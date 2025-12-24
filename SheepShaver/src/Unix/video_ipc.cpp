/*
 *  video_ipc.cpp - IPC-based video driver for SheepShaver web streaming
 *
 *  SheepShaver (C) 1997-2008 Marc Hellwig and Christian Bauer
 *  IPC mode (C) 2024
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

/*
 *  NOTES:
 *    Headless IPC video driver for SheepShaver web streaming.
 *    Outputs framebuffer to shared memory for WebRTC server to encode.
 *
 *    - Emulator creates SHM at /macemu-video-{PID}
 *    - Emulator creates Unix socket at /tmp/macemu-{PID}.sock
 *    - Emulator converts Mac framebuffer to BGRA
 *    - Server connects and reads frames for encoding
 */

#include "sysdeps.h"

#ifdef ENABLE_IPC_VIDEO

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>

// POSIX IPC
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>

// Byte shuffle for ARGB<->BGRA conversion (libyuv fallback)
#ifdef HAVE_LIBYUV
#include <libyuv.h>
#else
namespace libyuv {
static inline int ARGBToBGRA(
    const uint8_t* src, int src_stride,
    uint8_t* dst, int dst_stride,
    int width, int height)
{
    for (int y = 0; y < height; y++) {
        const uint8_t* s = src + y * src_stride;
        uint8_t* d = dst + y * dst_stride;
        for (int x = 0; x < width; x++) {
            d[x*4 + 0] = s[x*4 + 3];
            d[x*4 + 1] = s[x*4 + 2];
            d[x*4 + 2] = s[x*4 + 1];
            d[x*4 + 3] = s[x*4 + 0];
        }
    }
    return 0;
}
}
#endif

#include "main.h"
#include "adb.h"
#include "prefs.h"
#include "user_strings.h"
#include "video.h"
#include "video_defs.h"
#include "vm_alloc.h"

// IPC protocol definitions
#include "ipc_protocol.h"

#define DEBUG 0
#include "debug.h"

// Global variables from video.h (defined in video.cpp)
extern bool video_activated;
extern uint32 screen_base;
extern int cur_mode;
extern int display_type;
extern rgb_color mac_pal[256];
extern rgb_color mac_gamma[256];
extern uint8 remap_mac_be[256];
extern uint8 MacCursor[68];
extern VidLocals *private_data;
extern struct VideoInfo VModes[64];

static int num_modes = 0;

// Frame buffer
static uint8 *the_buffer = NULL;
static uint32 the_buffer_size = 0;

// Frame parameters
static uint32 frame_width = 0;
static uint32 frame_height = 0;
static uint32 frame_depth = 0;
static uint32 frame_bytes_per_row = 0;

// IPC resources
static MacEmuVideoBuffer *video_shm = NULL;
static int shm_fd = -1;
static std::string shm_name;

// Control socket
static int control_socket = -1;
static std::string socket_path;
static std::thread input_thread;
static std::atomic<bool> input_thread_running{false};

// Mouse position tracking (ADBMouseMoved expects absolute coordinates)
static int mouse_x = 0;
static int mouse_y = 0;

// Palette for indexed modes
static uint8 current_palette[256 * 3];

// Forward declarations
static bool init_ipc_resources(uint32 width, uint32 height);
static void cleanup_ipc_resources();
static void input_thread_func();
static void convert_frame_to_bgra();

/*
 *  Add a video mode to the list
 */
static void add_mode(int type, uint32 width, uint32 height, uint32 mode, uint32 id)
{
    if (num_modes >= 64) return;

    VModes[num_modes].viType = type;
    VModes[num_modes].viXsize = width;
    VModes[num_modes].viYsize = height;
    VModes[num_modes].viRowBytes = TrivialBytesPerRow(width, mode);
    VModes[num_modes].viAppleMode = mode;
    VModes[num_modes].viAppleID = id;
    num_modes++;
}

/*
 *  Initialize IPC shared memory and control socket
 */
static bool init_ipc_resources(uint32 width, uint32 height)
{
    pid_t pid = getpid();

    // Create SHM name
    char name_buf[64];
    snprintf(name_buf, sizeof(name_buf), "%s%d", MACEMU_VIDEO_SHM_PREFIX, pid);
    shm_name = name_buf;

    // Create shared memory
    shm_unlink(shm_name.c_str());  // Remove if exists
    shm_fd = shm_open(shm_name.c_str(), O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0) {
        D(bug("IPC: Failed to create SHM %s: %s\n", shm_name.c_str(), strerror(errno)));
        return false;
    }

    // Size the SHM
    if (ftruncate(shm_fd, sizeof(MacEmuVideoBuffer)) < 0) {
        D(bug("IPC: Failed to size SHM: %s\n", strerror(errno)));
        close(shm_fd);
        shm_unlink(shm_name.c_str());
        return false;
    }

    // Map SHM
    video_shm = (MacEmuVideoBuffer *)mmap(NULL, sizeof(MacEmuVideoBuffer),
                                           PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (video_shm == MAP_FAILED) {
        D(bug("IPC: Failed to map SHM: %s\n", strerror(errno)));
        close(shm_fd);
        shm_unlink(shm_name.c_str());
        return false;
    }

    // Initialize video buffer
    macemu_init_video_buffer(video_shm, pid, width, height);
    video_shm->state = MACEMU_STATE_RUNNING;

    D(bug("IPC: Created SHM %s, %dx%d\n", shm_name.c_str(), width, height));

    // Create control socket
    snprintf(name_buf, sizeof(name_buf), "%s%d%s",
             MACEMU_CONTROL_SOCK_PREFIX, pid, MACEMU_CONTROL_SOCK_SUFFIX);
    socket_path = name_buf;

    unlink(socket_path.c_str());  // Remove if exists

    control_socket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (control_socket < 0) {
        D(bug("IPC: Failed to create control socket: %s\n", strerror(errno)));
        return false;  // Non-fatal, continue without input
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(control_socket, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        D(bug("IPC: Failed to bind control socket: %s\n", strerror(errno)));
        close(control_socket);
        control_socket = -1;
    } else if (listen(control_socket, 1) < 0) {
        D(bug("IPC: Failed to listen on control socket: %s\n", strerror(errno)));
        close(control_socket);
        control_socket = -1;
    } else {
        D(bug("IPC: Control socket listening at %s\n", socket_path.c_str()));

        // Start input handling thread
        input_thread_running = true;
        input_thread = std::thread(input_thread_func);
    }

    printf("SheepShaver IPC video: SHM=%s Socket=%s\n", shm_name.c_str(), socket_path.c_str());

    return true;
}

/*
 *  Cleanup IPC resources
 */
static void cleanup_ipc_resources()
{
    // Stop input thread
    if (input_thread_running) {
        input_thread_running = false;
        if (control_socket >= 0) {
            shutdown(control_socket, SHUT_RDWR);
        }
        if (input_thread.joinable()) {
            input_thread.join();
        }
    }

    // Close control socket
    if (control_socket >= 0) {
        close(control_socket);
        control_socket = -1;
        unlink(socket_path.c_str());
    }

    // Mark as stopped
    if (video_shm) {
        video_shm->state = MACEMU_STATE_STOPPED;
    }

    // Unmap and close SHM
    if (video_shm && video_shm != MAP_FAILED) {
        munmap(video_shm, sizeof(MacEmuVideoBuffer));
        video_shm = NULL;
    }
    if (shm_fd >= 0) {
        close(shm_fd);
        shm_fd = -1;
    }
    if (!shm_name.empty()) {
        shm_unlink(shm_name.c_str());
        shm_name.clear();
    }
}

/*
 *  Input handling thread
 */
static void input_thread_func()
{
    int client_fd = -1;

    while (input_thread_running) {
        // Accept connection if none
        if (client_fd < 0) {
            struct pollfd pfd = { control_socket, POLLIN, 0 };
            if (poll(&pfd, 1, 100) > 0) {
                client_fd = accept(control_socket, NULL, NULL);
                if (client_fd >= 0) {
                    D(bug("IPC: Client connected\n"));
                }
            }
            continue;
        }

        // Read input
        MacEmuInput input;
        struct pollfd pfd = { client_fd, POLLIN, 0 };
        if (poll(&pfd, 1, 10) <= 0) continue;

        ssize_t n = recv(client_fd, &input, sizeof(input), MSG_WAITALL);
        if (n <= 0) {
            D(bug("IPC: Client disconnected\n"));
            close(client_fd);
            client_fd = -1;
            continue;
        }

        // Process input
        switch (input.hdr.type) {
            case MACEMU_INPUT_KEY:
                if (input.key.hdr.flags & MACEMU_KEY_DOWN) {
                    ADBKeyDown(input.key.mac_keycode);
                } else {
                    ADBKeyUp(input.key.mac_keycode);
                }
                break;

            case MACEMU_INPUT_MOUSE:
                // Accumulate deltas into absolute position (ADBMouseMoved expects absolute coords)
                mouse_x += input.mouse.x;
                mouse_y += input.mouse.y;
                // Clamp to screen bounds
                if (mouse_x < 0) mouse_x = 0;
                if (mouse_y < 0) mouse_y = 0;
                if (mouse_x >= (int)frame_width) mouse_x = frame_width - 1;
                if (mouse_y >= (int)frame_height) mouse_y = frame_height - 1;
                ADBMouseMoved(mouse_x, mouse_y);
                if (input.mouse.buttons & MACEMU_MOUSE_LEFT) {
                    ADBMouseDown(0);
                } else {
                    ADBMouseUp(0);
                }
                break;

            case MACEMU_INPUT_PING:
                // Echo ping timestamps to shared memory for latency measurement
                if (video_shm) {
                    video_shm->ping_timestamps.t1_browser_ms = input.ping.t1_browser_send_ms;
                    video_shm->ping_timestamps.t2_server_us = input.ping.t2_server_recv_us;
                    video_shm->ping_timestamps.t3_emulator_us = input.ping.t3_emulator_recv_us;
                    ATOMIC_STORE(video_shm->ping_sequence, input.ping.sequence);
                }
                break;
        }
    }

    if (client_fd >= 0) {
        close(client_fd);
    }
}

/*
 *  Convert Mac framebuffer to BGRA and signal frame complete
 */
static void convert_frame_to_bgra()
{
    if (!video_shm || !the_buffer) return;

    uint32 width = video_shm->width;
    uint32 height = video_shm->height;
    if (width == 0 || height == 0) return;

    uint32 write_idx = video_shm->write_index;
    uint8 *dst_frame = macemu_get_frame_ptr(video_shm, write_idx);
    int dst_stride = macemu_get_bgra_stride();

    video_shm->pixel_format = MACEMU_PIXFMT_BGRA;

    switch (frame_depth) {
        case 32: {
            // Mac 32-bit: bytes A,R,G,B -> convert to B,G,R,A
            libyuv::ARGBToBGRA(
                the_buffer, frame_bytes_per_row,
                dst_frame, dst_stride,
                width, height
            );
            break;
        }
        case 16: {
            // Mac big-endian RGB555 -> BGRA
            const uint8 *src = the_buffer;
            for (uint32 row = 0; row < height; row++) {
                const uint16 *src_row = (const uint16 *)(src + row * frame_bytes_per_row);
                uint8 *dst_row = dst_frame + row * dst_stride;
                for (uint32 x = 0; x < width; x++) {
                    uint16 pixel = src_row[x];
                    #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
                    pixel = (pixel >> 8) | (pixel << 8);
                    #endif
                    uint8 r = ((pixel >> 10) & 0x1F) << 3;
                    uint8 g = ((pixel >> 5) & 0x1F) << 3;
                    uint8 b = (pixel & 0x1F) << 3;
                    dst_row[x * 4 + 0] = b;
                    dst_row[x * 4 + 1] = g;
                    dst_row[x * 4 + 2] = r;
                    dst_row[x * 4 + 3] = 255;
                }
            }
            break;
        }
        case 8:
        case 4:
        case 2:
        case 1: {
            // Indexed color modes
            const uint8 *src = the_buffer;
            for (uint32 row = 0; row < height; row++) {
                const uint8 *src_row = src + row * frame_bytes_per_row;
                uint8 *dst_row = dst_frame + row * dst_stride;

                for (uint32 x = 0; x < width; x++) {
                    uint8 index = 0;
                    switch (frame_depth) {
                        case 8: index = src_row[x]; break;
                        case 4: index = (src_row[x / 2] >> (4 - (x % 2) * 4)) & 0x0F; break;
                        case 2: index = (src_row[x / 4] >> (6 - (x % 4) * 2)) & 0x03; break;
                        case 1: index = (src_row[x / 8] >> (7 - (x % 8))) & 0x01; break;
                    }

                    uint8 r = current_palette[index * 3 + 0];
                    uint8 g = current_palette[index * 3 + 1];
                    uint8 b = current_palette[index * 3 + 2];

                    dst_row[x * 4 + 0] = b;
                    dst_row[x * 4 + 1] = g;
                    dst_row[x * 4 + 2] = r;
                    dst_row[x * 4 + 3] = 255;
                }
            }
            break;
        }
    }

    // Get timestamp and signal frame complete
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t timestamp_us = (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;

    macemu_frame_complete(video_shm, timestamp_us);
}

/*
 *  Initialize video system
 */
bool VideoInit(void)
{
    D(bug("VideoInit (IPC mode)\n"));

    // Don't allocate private_data here - video.cpp handles it in VideoDoDriverIO()
    private_data = NULL;

    // Get requested resolution from prefs
    const char *mode_str = PrefsFindString("screen");
    int width = 800, height = 600, depth = 32;

    if (mode_str) {
        // Try various screen mode formats
        // IPC mode: ipc/width/height (no depth - always 32-bit)
        if (sscanf(mode_str, "ipc/%d/%d", &width, &height) == 2) {
            depth = 32;  // IPC always uses 32-bit
        } else if (sscanf(mode_str, "win/%d/%d/%d", &width, &height, &depth) != 3) {
            if (sscanf(mode_str, "dga/%d/%d/%d", &width, &height, &depth) != 3) {
                sscanf(mode_str, "%d/%d/%d", &width, &height, &depth);
            }
        }
    }

    // Clamp to maximum supported
    if (width > MACEMU_MAX_WIDTH) width = MACEMU_MAX_WIDTH;
    if (height > MACEMU_MAX_HEIGHT) height = MACEMU_MAX_HEIGHT;

    // Add video modes
    num_modes = 0;

    // Add requested resolution with all depths
    int id = APPLE_CUSTOM;
    add_mode(DIS_WINDOW, width, height, APPLE_1_BIT, id);
    add_mode(DIS_WINDOW, width, height, APPLE_2_BIT, id);
    add_mode(DIS_WINDOW, width, height, APPLE_4_BIT, id);
    add_mode(DIS_WINDOW, width, height, APPLE_8_BIT, id);
    add_mode(DIS_WINDOW, width, height, APPLE_16_BIT, id);
    add_mode(DIS_WINDOW, width, height, APPLE_32_BIT, id);

    // Terminate mode list
    VModes[num_modes].viType = DIS_INVALID;

    // Select initial mode based on requested depth
    cur_mode = 0;
    for (int i = 0; i < num_modes; i++) {
        if (VModes[i].viAppleMode == DepthModeForPixelDepth(depth)) {
            cur_mode = i;
            break;
        }
    }

    // Store frame parameters
    frame_width = VModes[cur_mode].viXsize;
    frame_height = VModes[cur_mode].viYsize;
    frame_bytes_per_row = VModes[cur_mode].viRowBytes;

    switch (VModes[cur_mode].viAppleMode) {
        case APPLE_1_BIT:  frame_depth = 1; break;
        case APPLE_2_BIT:  frame_depth = 2; break;
        case APPLE_4_BIT:  frame_depth = 4; break;
        case APPLE_8_BIT:  frame_depth = 8; break;
        case APPLE_16_BIT: frame_depth = 16; break;
        case APPLE_32_BIT: frame_depth = 32; break;
    }

    // Initialize mouse position to center of screen
    mouse_x = frame_width / 2;
    mouse_y = frame_height / 2;

    // Initialize IPC
    if (!init_ipc_resources(frame_width, frame_height)) {
        D(bug("IPC: Failed to initialize IPC resources\n"));
        return false;
    }

    // Allocate frame buffer at a specific Mac address
    // Must be outside RAM/ROM areas but within valid Mac address space
    // Use 0x60000000 as a safe video buffer address
    the_buffer_size = frame_bytes_per_row * frame_height;
    const uint32 VIDEO_BUFFER_MAC_ADDR = 0x60000000;

    // Allocate at the Mac address (will be VMBaseDiff + VIDEO_BUFFER_MAC_ADDR on host)
    if (vm_acquire_fixed(Mac2HostAddr(VIDEO_BUFFER_MAC_ADDR), the_buffer_size) != 0) {
        printf("IPC video: Failed to allocate video buffer at Mac address 0x%08x\n", VIDEO_BUFFER_MAC_ADDR);
        cleanup_ipc_resources();
        return false;
    }
    the_buffer = Mac2HostAddr(VIDEO_BUFFER_MAC_ADDR);
    memset(the_buffer, 0, the_buffer_size);

    // Set Mac frame buffer base
    screen_base = VIDEO_BUFFER_MAC_ADDR;

    // Initialize palette to grayscale
    for (int i = 0; i < 256; i++) {
        current_palette[i * 3 + 0] = i;
        current_palette[i * 3 + 1] = i;
        current_palette[i * 3 + 2] = i;
    }

    video_activated = true;
    display_type = DIS_WINDOW;

    D(bug("IPC VideoInit: %dx%d, depth=%d, screen_base=0x%08x\n",
          frame_width, frame_height, frame_depth, screen_base));

    return true;
}

/*
 *  Deinitialize video system
 */
void VideoExit(void)
{
    D(bug("VideoExit (IPC mode)\n"));

    video_activated = false;

    cleanup_ipc_resources();

    if (the_buffer) {
        // Release at the Mac address we allocated at
        const uint32 VIDEO_BUFFER_MAC_ADDR = 0x60000000;
        vm_release(Mac2HostAddr(VIDEO_BUFFER_MAC_ADDR), the_buffer_size);
        the_buffer = NULL;
    }
}

/*
 *  Video VBL interrupt - called periodically to refresh display
 */
void VideoVBL(void)
{
    if (!video_activated || !video_shm) return;

    // Convert frame and signal to server
    convert_frame_to_bgra();

    // Execute video VBL service (needed for Mac OS to process input events)
    if (private_data != NULL && private_data->interruptsEnabled)
        VSLDoInterruptService(private_data->vslServiceID);
}

/*
 *  Set palette (for indexed color modes)
 */
void video_set_palette(void)
{
    for (int i = 0; i < 256; i++) {
        current_palette[i * 3 + 0] = mac_pal[i].red >> 8;
        current_palette[i * 3 + 1] = mac_pal[i].green >> 8;
        current_palette[i * 3 + 2] = mac_pal[i].blue >> 8;
    }
}

void video_set_gamma(int n_colors) { }
void video_set_cursor(void) { }
bool video_can_change_cursor(void) { return false; }
void video_set_dirty_area(int x, int y, int w, int h) { }

/*
 *  Video mode change
 */
int16 video_mode_change(VidLocals *csSave, uint32 ParamPtr)
{
    // For now, reject mode changes
    return paramErr;
}

/*
 *  Quit full screen mode (called from video.cpp)
 */
void VideoQuitFullScreen(void)
{
    // Nothing to do in IPC mode
}

#endif // ENABLE_IPC_VIDEO
