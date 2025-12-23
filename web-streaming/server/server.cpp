/*
 * Standalone WebRTC Server for macemu (BasiliskII / SheepShaver)
 *
 * Architecture (v4):
 * - Server CONNECTS to emulator resources by PID
 * - Emulator creates SHM (/macemu-video-{PID}) and socket (/tmp/macemu-{PID}.sock)
 * - Emulator converts Mac framebuffer to BGRA (B,G,R,A bytes = libyuv "ARGB")
 * - Server encodes to H.264 (via I420 conversion) or PNG (via RGB conversion)
 * - Server handles browser keycode to Mac keycode conversion
 */

#include "ipc_protocol.h"
#include "codec.h"
#include "h264_encoder.h"
#include "png_encoder.h"

#include <rtc/rtc.hpp>
#include <rtc/h264rtppacketizer.hpp>
#include <rtc/rtppacketizationconfig.hpp>
#include <rtc/frameinfo.hpp>

#include <string>
#include <memory>
#include <mutex>
#include <atomic>
#include <map>
#include <vector>
#include <thread>
#include <chrono>
#include <functional>
#include <sstream>
#include <fstream>
#include <csignal>
#include <iomanip>

// POSIX IPC and process management
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>
#include <pwd.h>
#include <poll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <libgen.h>

#include <sys/epoll.h>
#include <sys/eventfd.h>

// Configuration
static int g_http_port = 8000;
static int g_signaling_port = 8090;
static std::string g_roms_path = "storage/roms";
static std::string g_images_path = "storage/images";
static std::string g_prefs_path = "basilisk_ii.prefs";
static std::string g_emulator_path;    // Path to BasiliskII/SheepShaver executable
static bool g_auto_start_emulator = true;
static pid_t g_target_emulator_pid = 0;  // If specified, connect to this PID
static CodecType g_server_codec = CodecType::PNG;  // Server-side codec preference (default: PNG)

// Debug/verbosity flags (extern in encoders for cross-module access)
static bool g_debug_connection = false;  // WebRTC, ICE, signaling logs
bool g_debug_mode_switch = false;        // Mode/resolution/color depth changes (non-static for encoders)
static bool g_debug_perf = false;        // Performance stats, ping logs
static bool g_debug_frames = false;      // Save frame dumps to disk (.ppm files)

// Global state
static std::atomic<bool> g_running(true);
static std::atomic<bool> g_emulator_connected(false);
static std::atomic<bool> g_restart_emulator_requested(false);
static pid_t g_emulator_pid = -1;

// IPC handles - server connects to emulator's resources
static MacEmuVideoBuffer* g_video_shm = nullptr;
static int g_video_shm_fd = -1;
static int g_control_socket = -1;
static int g_frame_ready_eventfd = -1;  // Server's copy of eventfd for frame notifications
static std::string g_connected_shm_name;
static std::string g_connected_socket_path;

// Input event counters (for stats)
static std::atomic<uint64_t> g_mouse_move_count(0);
static std::atomic<uint64_t> g_mouse_click_count(0);
static std::atomic<uint64_t> g_key_count(0);


// Global flag to request keyframe (set when new peer connects)
static std::atomic<bool> g_request_keyframe(false);

// Forward declarations
class WebRTCServer;

// Signal handler
static void signal_handler(int sig) {
    fprintf(stderr, "\nServer: Received signal %d, shutting down...\n", sig);
    g_running = false;
}


/*
 * Browser keycode to Mac ADB keycode conversion
 * This was moved from emulator to server per the new architecture
 */

static int browser_to_mac_keycode(int keycode) {
    if (keycode >= 65 && keycode <= 90) {
        static const int letter_map[] = {
            0x00, 0x0B, 0x08, 0x02, 0x0E, 0x03, 0x05, 0x04,
            0x22, 0x26, 0x28, 0x25, 0x2E, 0x2D, 0x1F, 0x23,
            0x0C, 0x0F, 0x01, 0x11, 0x20, 0x09, 0x0D, 0x07,
            0x10, 0x06
        };
        return letter_map[keycode - 65];
    } else if (keycode >= 48 && keycode <= 57) {
        static const int number_map[] = {
            0x1D, 0x12, 0x13, 0x14, 0x15, 0x17, 0x16, 0x1A, 0x1C, 0x19
        };
        return number_map[keycode - 48];
    } else {
        switch (keycode) {
            case 8: return 0x33;   // Backspace
            case 9: return 0x30;   // Tab
            case 13: return 0x24;  // Enter
            case 16: return 0x38;  // Shift
            case 17: return 0x36;  // Ctrl -> Command
            case 18: return 0x3A;  // Alt -> Option
            case 27: return 0x35;  // Escape
            case 32: return 0x31;  // Space
            case 37: return 0x3B;  // Left
            case 38: return 0x3E;  // Up
            case 39: return 0x3C;  // Right
            case 40: return 0x3D;  // Down
            case 46: return 0x75;  // Delete
            case 91: return 0x37;  // Meta -> Command
            case 186: return 0x29; // ;
            case 187: return 0x18; // =
            case 188: return 0x2B; // ,
            case 189: return 0x1B; // -
            case 190: return 0x2F; // .
            case 191: return 0x2C; // /
            case 192: return 0x32; // `
            case 219: return 0x21; // [
            case 220: return 0x2A; // backslash
            case 221: return 0x1E; // ]
            case 222: return 0x27; // '
            default: return -1;
        }
    }
}


/*
 * JSON helpers
 */

static std::string json_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out += c;
    }
    return out;
}

static std::string json_get_string(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";

    pos = json.find(':', pos);
    if (pos == std::string::npos) return "";

    pos = json.find('"', pos);
    if (pos == std::string::npos) return "";
    pos++;

    // Parse the JSON string with proper unescaping
    std::string result;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            pos++;
            switch (json[pos]) {
                case 'n': result += '\n'; break;
                case 'r': result += '\r'; break;
                case 't': result += '\t'; break;
                case '"': result += '"'; break;
                case '\\': result += '\\'; break;
                default: result += json[pos]; break;
            }
        } else {
            result += json[pos];
        }
        pos++;
    }

    return result;
}


/*
 * IPC: Connect to emulator's shared memory by PID
 */

static bool connect_to_video_shm(pid_t pid) {
    std::string shm_name = std::string(MACEMU_VIDEO_SHM_PREFIX) + std::to_string(pid);

    g_video_shm_fd = shm_open(shm_name.c_str(), O_RDONLY, 0);
    if (g_video_shm_fd < 0) {
        // Not an error during scanning - emulator may not exist yet
        return false;
    }

    // Map shared memory (read-only for server)
    g_video_shm = (MacEmuVideoBuffer*)mmap(nullptr, sizeof(MacEmuVideoBuffer),
                                            PROT_READ, MAP_SHARED,
                                            g_video_shm_fd, 0);
    if (g_video_shm == MAP_FAILED) {
        fprintf(stderr, "IPC: Failed to map video SHM for PID %d: %s\n", pid, strerror(errno));
        close(g_video_shm_fd);
        g_video_shm_fd = -1;
        g_video_shm = nullptr;
        return false;
    }

    // Validate
    int result = macemu_validate_video_buffer(g_video_shm, pid);
    if (result != 0) {
        fprintf(stderr, "IPC: SHM validation failed for PID %d (error %d)\n", pid, result);
        munmap(g_video_shm, sizeof(MacEmuVideoBuffer));
        close(g_video_shm_fd);
        g_video_shm_fd = -1;
        g_video_shm = nullptr;
        return false;
    }

    g_connected_shm_name = shm_name;
    g_emulator_pid = pid;
    fprintf(stderr, "IPC: Connected to video SHM '%s' (%dx%d)\n",
            shm_name.c_str(), g_video_shm->width, g_video_shm->height);
    return true;
}

static void disconnect_video_shm() {
    if (g_video_shm && g_video_shm != MAP_FAILED) {
        munmap(g_video_shm, sizeof(MacEmuVideoBuffer));
        g_video_shm = nullptr;
    }
    if (g_video_shm_fd >= 0) {
        close(g_video_shm_fd);
        g_video_shm_fd = -1;
    }
    g_connected_shm_name.clear();
}


/*
 * IPC: Connect to emulator's control socket by PID
 */

static bool connect_to_control_socket(pid_t pid) {
    std::string socket_path = std::string(MACEMU_CONTROL_SOCK_PREFIX) + std::to_string(pid) +
                              std::string(MACEMU_CONTROL_SOCK_SUFFIX);

    g_control_socket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_control_socket < 0) {
        fprintf(stderr, "IPC: Failed to create socket: %s\n", strerror(errno));
        return false;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(g_control_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(g_control_socket);
        g_control_socket = -1;
        return false;
    }

    // Set non-blocking
    int flags = fcntl(g_control_socket, F_GETFL, 0);
    fcntl(g_control_socket, F_SETFL, flags | O_NONBLOCK);

    g_connected_socket_path = socket_path;
    g_emulator_connected = true;
    fprintf(stderr, "IPC: Connected to control socket '%s'\n", socket_path.c_str());

    // Receive eventfd from emulator via SCM_RIGHTS for low-latency frame notification
    // The emulator sends this immediately after accepting the connection
    struct msghdr msg = {};
    struct cmsghdr *cmsg;
    char buf[CMSG_SPACE(sizeof(int))];
    char data;
    struct iovec iov = { &data, 1 };

    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = buf;
    msg.msg_controllen = sizeof(buf);

    // Try to receive the eventfd (with short timeout since it's sent immediately)
    fcntl(g_control_socket, F_SETFL, flags & ~O_NONBLOCK);  // Temporarily blocking
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(g_control_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ssize_t n = recvmsg(g_control_socket, &msg, 0);
    if (n > 0 && data == 'E') {
        for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
                memcpy(&g_frame_ready_eventfd, CMSG_DATA(cmsg), sizeof(int));
                fprintf(stderr, "IPC: Received eventfd %d from emulator for low-latency sync\n", g_frame_ready_eventfd);
                break;
            }
        }
    }

    // Restore non-blocking mode
    fcntl(g_control_socket, F_SETFL, flags | O_NONBLOCK);

    return true;
}

static void disconnect_control_socket() {
    if (g_control_socket >= 0) {
        close(g_control_socket);
        g_control_socket = -1;
    }
    if (g_frame_ready_eventfd >= 0) {
        close(g_frame_ready_eventfd);
        g_frame_ready_eventfd = -1;
    }
    g_emulator_connected = false;
    g_connected_socket_path.clear();
}


/*
 * Send binary input to emulator
 */

static bool send_key_input(int mac_keycode, bool down) {
    if (g_control_socket < 0) return false;

    MacEmuKeyInput msg;
    msg.hdr.type = MACEMU_INPUT_KEY;
    msg.hdr.flags = down ? MACEMU_KEY_DOWN : MACEMU_KEY_UP;
    msg.hdr._reserved = 0;
    msg.mac_keycode = mac_keycode;
    msg.modifiers = 0;  // TODO: track modifier state
    msg._reserved = 0;

    return send(g_control_socket, &msg, sizeof(msg), MSG_NOSIGNAL) == sizeof(msg);
}

static bool send_mouse_input(int dx, int dy, uint8_t buttons, uint64_t browser_timestamp_ms) {
    if (g_control_socket < 0) return false;

    MacEmuMouseInput msg;
    msg.hdr.type = MACEMU_INPUT_MOUSE;
    msg.hdr.flags = 0;
    msg.hdr._reserved = 0;
    msg.x = dx;
    msg.y = dy;
    msg.buttons = buttons;
    memset(msg._reserved, 0, sizeof(msg._reserved));
    msg.timestamp_ms = browser_timestamp_ms;

    return send(g_control_socket, &msg, sizeof(msg), MSG_NOSIGNAL) == sizeof(msg);
}

static bool send_command(uint8_t command) {
    if (g_control_socket < 0) return false;

    MacEmuCommandInput msg;
    msg.hdr.type = MACEMU_INPUT_COMMAND;
    msg.hdr.flags = 0;
    msg.hdr._reserved = 0;
    msg.command = command;
    memset(msg._reserved, 0, sizeof(msg._reserved));

    return send(g_control_socket, &msg, sizeof(msg), MSG_NOSIGNAL) == sizeof(msg);
}

static bool send_ping_input(uint32_t sequence, uint64_t t1_browser_send_ms) {
    if (g_control_socket < 0) return false;

    // Add server receive timestamp (t2)
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t t2_server_recv_us = (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;

    MacEmuPingInput msg;
    msg.hdr.type = MACEMU_INPUT_PING;
    msg.hdr.flags = 0;
    msg.hdr._reserved = 0;
    msg.sequence = sequence;
    msg.t1_browser_send_ms = t1_browser_send_ms;
    msg.t2_server_recv_us = t2_server_recv_us;
    msg.t3_emulator_recv_us = 0;  // Will be filled by emulator

    return send(g_control_socket, &msg, sizeof(msg), MSG_NOSIGNAL) == sizeof(msg);
}


/*
 * Try to connect to a running emulator
 */

static bool try_connect_to_emulator(pid_t pid) {
    // First try to connect to SHM
    if (!connect_to_video_shm(pid)) {
        return false;
    }

    // Then try to connect to control socket
    if (!connect_to_control_socket(pid)) {
        disconnect_video_shm();
        return false;
    }

    return true;
}

// Forward declaration - implementation after WebRTCServer class
static void disconnect_from_emulator(WebRTCServer* webrtc = nullptr);


/*
 * Scan for running emulators (look for SHM files)
 */

static std::vector<pid_t> scan_for_emulators() {
    std::vector<pid_t> pids;

    DIR* dir = opendir("/dev/shm");
    if (!dir) return pids;

    struct dirent* entry;
    const char* prefix = "macemu-video-";
    size_t prefix_len = strlen(prefix);

    while ((entry = readdir(dir)) != nullptr) {
        if (strncmp(entry->d_name, prefix, prefix_len) == 0) {
            pid_t pid = atoi(entry->d_name + prefix_len);
            if (pid > 0) {
                // Check if process still exists
                if (kill(pid, 0) == 0) {
                    pids.push_back(pid);
                }
            }
        }
    }
    closedir(dir);

    return pids;
}


/*
 * Emulator Process Management (for when server starts emulator)
 */

static std::string find_emulator() {
    // If path explicitly set, use it
    if (!g_emulator_path.empty()) {
        if (access(g_emulator_path.c_str(), X_OK) == 0) {
            return g_emulator_path;
        }
        fprintf(stderr, "Emulator: Specified path not executable: %s\n", g_emulator_path.c_str());
        return "";
    }

    // Look for emulator in bin/ subdirectory only
    const char* candidates[] = {
        "./bin/BasiliskII",
        "./bin/SheepShaver",
        nullptr
    };

    for (int i = 0; candidates[i]; i++) {
        if (access(candidates[i], X_OK) == 0) {
            char* resolved = realpath(candidates[i], nullptr);
            if (resolved) {
                std::string path(resolved);
                free(resolved);
                return path;
            }
        }
    }

    return "";
}

static pid_t g_started_emulator_pid = -1;  // PID of emulator we started

static bool start_emulator() {
    if (g_started_emulator_pid > 0) {
        // Already started one, check if still alive
        int status;
        pid_t result = waitpid(g_started_emulator_pid, &status, WNOHANG);
        if (result == 0) {
            // Still running
            return true;
        }
        // Exited
        g_started_emulator_pid = -1;
    }

    std::string emu_path = find_emulator();
    if (emu_path.empty()) {
        fprintf(stderr, "Emulator: No emulator found. Place BasiliskII or SheepShaver in current directory.\n");
        return false;
    }

    fprintf(stderr, "Emulator: Starting %s --config %s\n", emu_path.c_str(), g_prefs_path.c_str());

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "Emulator: Fork failed: %s\n", strerror(errno));
        return false;
    }

    if (pid == 0) {
        // Child process

        // Close server's file descriptors
        for (int fd = 3; fd < 1024; fd++) {
            close(fd);
        }

        // Pass debug flags to emulator via environment variables
        if (g_debug_mode_switch) setenv("MACEMU_DEBUG_MODE_SWITCH", "1", 1);
        if (g_debug_perf) setenv("MACEMU_DEBUG_PERF", "1", 1);
        if (g_debug_frames) setenv("MACEMU_DEBUG_FRAMES", "1", 1);

        // Execute emulator with prefs file
        // BasiliskII uses --config, SheepShaver uses --prefs
        if (emu_path.find("SheepShaver") != std::string::npos) {
            execl(emu_path.c_str(), emu_path.c_str(),
                  "--prefs", g_prefs_path.c_str(), nullptr);
        } else {
            execl(emu_path.c_str(), emu_path.c_str(),
                  "--config", g_prefs_path.c_str(), nullptr);
        }

        // If exec fails
        fprintf(stderr, "Emulator: Exec failed: %s\n", strerror(errno));
        _exit(1);
    }

    // Parent process
    g_started_emulator_pid = pid;
    fprintf(stderr, "Emulator: Started with PID %d\n", pid);
    return true;
}

static void stop_emulator() {
    if (g_started_emulator_pid <= 0) return;

    fprintf(stderr, "Emulator: Stopping PID %d\n", g_started_emulator_pid);

    // Try graceful shutdown first via control socket
    if (g_control_socket >= 0) {
        send_command(MACEMU_CMD_STOP);
    }

    // Also send SIGTERM
    kill(g_started_emulator_pid, SIGTERM);

    // Wait up to 3 seconds
    for (int i = 0; i < 30; i++) {
        int status;
        pid_t result = waitpid(g_started_emulator_pid, &status, WNOHANG);
        if (result != 0) {
            g_started_emulator_pid = -1;
            fprintf(stderr, "Emulator: Stopped\n");
            return;
        }
        usleep(100000);  // 100ms
    }

    // Force kill
    fprintf(stderr, "Emulator: Force killing\n");
    kill(g_started_emulator_pid, SIGKILL);
    waitpid(g_started_emulator_pid, nullptr, 0);
    g_started_emulator_pid = -1;
}

// Returns: 0 if still running, -1 if not running/error, positive if exited with code
static int check_emulator_status() {
    if (g_started_emulator_pid <= 0) return -1;

    int status;
    pid_t result = waitpid(g_started_emulator_pid, &status, WNOHANG);
    if (result > 0) {
        // Emulator exited
        int exit_code = -1;
        if (WIFEXITED(status)) {
            exit_code = WEXITSTATUS(status);
            fprintf(stderr, "Emulator: Exited with code %d\n", exit_code);
            if (exit_code == 75) {
                fprintf(stderr, "Emulator: Restart requested (exit code 75)\n");
            }
        } else if (WIFSIGNALED(status)) {
            fprintf(stderr, "Emulator: Killed by signal %d\n", WTERMSIG(status));
        }
        g_started_emulator_pid = -1;
        g_emulator_connected = false;
        disconnect_from_emulator();
        return exit_code >= 0 ? exit_code : -1;
    } else if (result == 0) {
        return 0;  // Still running
    }
    return -1;  // Error
}


/*
 * Storage scanning and config
 */

static bool has_extension(const std::string& filename, const std::vector<std::string>& extensions) {
    size_t dot = filename.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = filename.substr(dot);
    for (auto& c : ext) c = tolower(c);
    for (const auto& e : extensions) {
        if (ext == e) return true;
    }
    return false;
}

struct FileInfo {
    std::string name;
    int64_t size;
    uint32_t checksum;
    bool has_checksum;
};

static uint32_t read_rom_checksum(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return 0;
    uint8_t buf[4];
    if (fread(buf, 1, 4, f) != 4) {
        fclose(f);
        return 0;
    }
    fclose(f);
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
}

static void scan_directory_recursive(const std::string& base_dir, const std::string& relative_path,
                                     const std::vector<std::string>& extensions, bool read_checksums,
                                     std::vector<FileInfo>& files) {
    std::string current_dir = relative_path.empty() ? base_dir : base_dir + "/" + relative_path;

    DIR* dir = opendir(current_dir.c_str());
    if (!dir) return;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;

        std::string name = entry->d_name;
        std::string full_path = current_dir + "/" + name;
        std::string rel_name = relative_path.empty() ? name : relative_path + "/" + name;

        struct stat st;
        if (stat(full_path.c_str(), &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            scan_directory_recursive(base_dir, rel_name, extensions, read_checksums, files);
        } else if (S_ISREG(st.st_mode)) {
            if (has_extension(name, extensions)) {
                FileInfo info;
                info.name = rel_name;
                info.size = st.st_size;
                info.checksum = 0;
                info.has_checksum = false;

                if (read_checksums) {
                    info.checksum = read_rom_checksum(full_path);
                    info.has_checksum = true;
                }

                files.push_back(info);
            }
        }
    }
    closedir(dir);
}

static std::vector<FileInfo> scan_directory(const std::string& directory,
                                            const std::vector<std::string>& extensions,
                                            bool read_checksums = false, bool recursive = false) {
    std::vector<FileInfo> files;

    if (recursive) {
        scan_directory_recursive(directory, "", extensions, read_checksums, files);
    } else {
        DIR* dir = opendir(directory.c_str());
        if (!dir) return files;

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_name[0] == '.') continue;

            std::string name = entry->d_name;
            if (has_extension(name, extensions)) {
                FileInfo info;
                info.name = name;
                info.size = 0;
                info.checksum = 0;
                info.has_checksum = false;

                std::string full_path = directory + "/" + name;
                struct stat st;
                if (stat(full_path.c_str(), &st) == 0) {
                    info.size = st.st_size;
                }

                if (read_checksums) {
                    info.checksum = read_rom_checksum(full_path);
                    info.has_checksum = true;
                }

                files.push_back(info);
            }
        }
        closedir(dir);
    }

    std::sort(files.begin(), files.end(), [](const FileInfo& a, const FileInfo& b) {
        return a.name < b.name;
    });
    return files;
}

static std::string get_storage_json() {
    auto roms = scan_directory(g_roms_path, {".rom"}, true, true);
    auto disks = scan_directory(g_images_path, {".img", ".dsk", ".hfv", ".toast"});
    auto cdroms = scan_directory(g_images_path, {".iso"});

    std::ostringstream json;
    json << "{\n";
    json << "  \"romsPath\": \"" << json_escape(g_roms_path) << "\",\n";
    json << "  \"imagesPath\": \"" << json_escape(g_images_path) << "\",\n";
    json << "  \"roms\": [";
    for (size_t i = 0; i < roms.size(); i++) {
        if (i > 0) json << ", ";
        json << "{\"name\": \"" << json_escape(roms[i].name) << "\", \"size\": " << roms[i].size;
        char checksum_hex[16];
        snprintf(checksum_hex, sizeof(checksum_hex), "%08x", roms[i].checksum);
        json << ", \"checksum\": \"" << checksum_hex << "\"}";
    }
    json << "],\n";
    json << "  \"disks\": [";
    for (size_t i = 0; i < disks.size(); i++) {
        if (i > 0) json << ", ";
        json << "{\"name\": \"" << json_escape(disks[i].name) << "\", \"size\": " << disks[i].size << "}";
    }
    json << "],\n";
    json << "  \"cdroms\": [";
    for (size_t i = 0; i < cdroms.size(); i++) {
        if (i > 0) json << ", ";
        json << "{\"name\": \"" << json_escape(cdroms[i].name) << "\", \"size\": " << cdroms[i].size << "}";
    }
    json << "]\n";
    json << "}";

    return json.str();
}

// Write raw prefs file content (JS frontend handles all serialization)
static bool write_prefs_file(const std::string& content) {
    std::ofstream file(g_prefs_path);
    if (!file) {
        fprintf(stderr, "Config: Failed to open prefs file for writing: %s\n", g_prefs_path.c_str());
        return false;
    }

    file << content;
    file.close();

    fprintf(stderr, "Config: Wrote prefs file: %s (%zu bytes)\n", g_prefs_path.c_str(), content.size());
    return true;
}

// Read raw prefs file content (JS frontend handles all parsing)
static std::string read_prefs_file() {
    std::ifstream file(g_prefs_path);
    if (!file) {
        return "";  // Empty string means no file exists
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// Create minimal prefs file if it doesn't exist
// Based on template from client.js
static void create_minimal_prefs_if_needed() {
    // Check if file already exists
    std::ifstream check(g_prefs_path);
    if (check.good()) {
        check.close();
        return;  // File exists, nothing to do
    }

    fprintf(stderr, "Config: Creating minimal prefs file at %s\n", g_prefs_path.c_str());

    // Minimal configuration template - matches client.js PREFS_TEMPLATE
    const char* minimal_prefs =
        "# Basilisk II preferences - minimal config for cold boot\n"
        "\n"
        "# ROM file (configure via web UI)\n"
        "rom \n"
        "\n"
        "# Hardware settings\n"
        "ramsize 33554432\n"  // 32 MB
        "screen ipc/800/600\n"
        "cpu 4\n"
        "modelid 14\n"
        "fpu true\n"
        "jit true\n"
        "nosound false\n"
        "\n"
        "# Video codec for web streaming (png or h264)\n"
        "webcodec png\n"
        "\n"
        "# JIT settings\n"
        "jitfpu true\n"
        "jitcachesize 8192\n"
        "jitlazyflush true\n"
        "jitinline true\n"
        "jitdebug false\n"
        "\n"
        "# Display settings\n"
        "displaycolordepth 0\n"
        "frameskip 0\n"
        "scale_nearest false\n"
        "scale_integer false\n"
        "\n"
        "# Input settings\n"
        "keyboardtype 5\n"
        "keycodes false\n"
        "mousewheelmode 1\n"
        "mousewheellines 3\n"
        "swap_opt_cmd true\n"
        "hotkey 0\n"
        "\n"
        "# Serial/Network\n"
        "seriala /dev/null\n"
        "serialb /dev/null\n"
        "udptunnel false\n"
        "udpport 6066\n"
        "etherpermanentaddress true\n"
        "ethermulticastmode 0\n"
        "routerenabled false\n"
        "ftp_port_list 21\n"
        "\n"
        "# Boot settings\n"
        "bootdrive 0\n"
        "bootdriver 0\n"
        "nocdrom false\n"
        "\n"
        "# System settings\n"
        "ignoresegv true\n"
        "idlewait true\n"
        "noclipconversion false\n"
        "nogui true\n"
        "sound_buffer 0\n"
        "name_encoding 0\n"
        "delay 0\n"
        "init_grab false\n"
        "yearofs 0\n"
        "dayofs 0\n"
        "reservewindowskey false\n"
        "\n"
        "# ExtFS settings\n"
        "enableextfs false\n"
        "debugextfs false\n"
        "extfs ./storage\n"
        "extdrives CDEFGHIJKLMNOPQRSTUVWXYZ\n"
        "pollmedia true\n";

    if (!write_prefs_file(minimal_prefs)) {
        fprintf(stderr, "Config: Failed to create minimal prefs file\n");
    }
}

// Read webcodec preference from prefs file
static void read_webcodec_pref() {
    std::ifstream file(g_prefs_path);
    if (!file) {
        fprintf(stderr, "Config: No prefs file, defaulting to PNG codec\n");
        g_server_codec = CodecType::PNG;
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') continue;

        // Look for "webcodec" preference
        if (line.rfind("webcodec ", 0) == 0) {
            std::string value = line.substr(9);
            // Trim whitespace
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
                value.pop_back();
            }

            if (value == "h264" || value == "H264") {
                g_server_codec = CodecType::H264;
                fprintf(stderr, "Config: webcodec = h264\n");
            } else if (value == "png" || value == "PNG") {
                g_server_codec = CodecType::PNG;
                fprintf(stderr, "Config: webcodec = png\n");
            } else {
                fprintf(stderr, "Config: Unknown webcodec '%s', defaulting to PNG\n", value.c_str());
                g_server_codec = CodecType::PNG;
            }
            return;
        }
    }

    // Not found - default to PNG
    fprintf(stderr, "Config: webcodec not set, defaulting to PNG\n");
    g_server_codec = CodecType::PNG;
}


/*
 * HTTP Server
 */

class HTTPServer {
public:
    bool start(int port) {
        port_ = port;

        server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd_ < 0) {
            fprintf(stderr, "HTTP: Failed to create socket\n");
            return false;
        }

        int opt = 1;
        setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        int flags = fcntl(server_fd_, F_GETFL, 0);
        fcntl(server_fd_, F_SETFL, flags | O_NONBLOCK);

        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        if (bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            fprintf(stderr, "HTTP: Failed to bind port %d\n", port);
            close(server_fd_);
            server_fd_ = -1;
            return false;
        }

        if (listen(server_fd_, 10) < 0) {
            fprintf(stderr, "HTTP: Failed to listen\n");
            close(server_fd_);
            server_fd_ = -1;
            return false;
        }

        running_ = true;
        thread_ = std::thread(&HTTPServer::run, this);

        fprintf(stderr, "HTTP: Server on port %d\n", port);
        return true;
    }

    void stop() {
        running_ = false;
        if (server_fd_ >= 0) {
            close(server_fd_);
            server_fd_ = -1;
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    void run() {
        while (running_ && g_running) {
            struct pollfd pfd;
            pfd.fd = server_fd_;
            pfd.events = POLLIN;

            int ret = poll(&pfd, 1, 100);
            if (ret <= 0) continue;

            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(server_fd_, (struct sockaddr*)&client_addr, &client_len);
            if (client_fd < 0) continue;

            handle_client(client_fd);
            close(client_fd);
        }
    }

    void handle_client(int fd) {
        char buffer[8192];
        ssize_t n = recv(fd, buffer, sizeof(buffer) - 1, 0);
        if (n <= 0) return;
        buffer[n] = '\0';

        std::string request(buffer);
        std::string method;
        std::string path = "/";

        size_t method_end = request.find(' ');
        if (method_end != std::string::npos) {
            method = request.substr(0, method_end);
            size_t path_end = request.find(' ', method_end + 1);
            if (path_end != std::string::npos) {
                path = request.substr(method_end + 1, path_end - method_end - 1);
            }
        }

        // Strip query string from path
        size_t query_pos = path.find('?');
        if (query_pos != std::string::npos) {
            path = path.substr(0, query_pos);
        }

        // Extract request body for POST requests
        std::string body;
        size_t body_start = request.find("\r\n\r\n");
        if (body_start != std::string::npos) {
            body = request.substr(body_start + 4);
        }

        // API endpoints
        if (path == "/api/config" && method == "GET") {
            // Return debug configuration flags
            std::ostringstream json;
            json << "{";
            json << "\"debug_connection\": " << (g_debug_connection ? "true" : "false");
            json << ", \"debug_mode_switch\": " << (g_debug_mode_switch ? "true" : "false");
            json << ", \"debug_perf\": " << (g_debug_perf ? "true" : "false");
            json << "}";
            send_json_response(fd, json.str());
            return;
        }

        if (path == "/api/storage" && method == "GET") {
            std::string json_body = get_storage_json();
            send_json_response(fd, json_body);
            return;
        }

        if (path == "/api/prefs" && method == "GET") {
            // Return raw prefs file content
            std::string prefs_content = read_prefs_file();
            std::string json_body = "{\"content\": \"" + json_escape(prefs_content) + "\", ";
            json_body += "\"path\": \"" + json_escape(g_prefs_path) + "\", ";
            json_body += "\"romsPath\": \"" + json_escape(g_roms_path) + "\", ";
            json_body += "\"imagesPath\": \"" + json_escape(g_images_path) + "\"}";
            send_json_response(fd, json_body);
            return;
        }

        if (path == "/api/prefs" && method == "POST") {
            // Write raw prefs file content from JSON body
            std::string content = json_get_string(body, "content");
            if (write_prefs_file(content)) {
                send_json_response(fd, "{\"success\": true}");
            } else {
                send_json_response(fd, "{\"success\": false, \"error\": \"Failed to write prefs file\"}");
            }
            return;
        }

        if (path == "/api/restart" && method == "POST") {
            fprintf(stderr, "Server: Restart requested via API\n");
            send_command(MACEMU_CMD_RESET);
            std::string json_body = "{\"success\": true, \"message\": \"Restart sent to emulator\"}";
            send_json_response(fd, json_body);
            return;
        }

        if (path == "/api/status" && method == "GET") {
            std::ostringstream json;
            json << "{";
            json << "\"emulator_connected\": " << (g_emulator_connected ? "true" : "false");
            json << ", \"emulator_running\": " << (g_started_emulator_pid > 0 ? "true" : "false");
            json << ", \"emulator_pid\": " << g_emulator_pid;
            if (g_video_shm) {
                json << ", \"video\": {\"width\": " << g_video_shm->width;
                json << ", \"height\": " << g_video_shm->height;
                json << ", \"frame_count\": " << g_video_shm->frame_count;  // Plain read, stats only
                json << ", \"state\": " << g_video_shm->state << "}";

                // Mouse latency from emulator (atomic - can be updated by stats thread)
                uint32_t latency_x10 = ATOMIC_LOAD(g_video_shm->mouse_latency_avg_ms);
                uint32_t latency_samples = ATOMIC_LOAD(g_video_shm->mouse_latency_samples);
                json << ", \"mouse_latency_ms\": " << std::fixed << std::setprecision(1) << (latency_x10 / 10.0);
                json << ", \"mouse_latency_samples\": " << latency_samples;
            }
            json << "}";
            send_json_response(fd, json.str());
            return;
        }

        if (path == "/api/emulator/start" && method == "POST") {
            std::string json_body;
            if (g_started_emulator_pid > 0) {
                json_body = "{\"success\": false, \"message\": \"Emulator already running\", \"pid\": " + std::to_string(g_started_emulator_pid) + "}";
            } else if (start_emulator()) {
                json_body = "{\"success\": true, \"message\": \"Emulator started\", \"pid\": " + std::to_string(g_started_emulator_pid) + "}";
            } else {
                json_body = "{\"success\": false, \"message\": \"Failed to start emulator\"}";
            }
            send_json_response(fd, json_body);
            return;
        }

        if (path == "/api/emulator/stop" && method == "POST") {
            std::string json_body;
            if (g_started_emulator_pid <= 0 && g_emulator_pid <= 0) {
                json_body = "{\"success\": false, \"message\": \"Emulator not running\"}";
            } else {
                if (g_started_emulator_pid > 0) {
                    stop_emulator();
                } else {
                    // Just disconnect from external emulator
                    send_command(MACEMU_CMD_STOP);
                    disconnect_from_emulator();
                }
                json_body = "{\"success\": true, \"message\": \"Emulator stopped\"}";
            }
            send_json_response(fd, json_body);
            return;
        }

        if (path == "/api/emulator/restart" && method == "POST") {
            g_restart_emulator_requested = true;
            std::string json_body = "{\"success\": true, \"message\": \"Restart requested\"}";
            send_json_response(fd, json_body);
            return;
        }

        if (path == "/api/log" && method == "POST") {
            // Client logging endpoint - parse and display browser logs
            std::string level = json_get_string(body, "level");
            std::string msg = json_get_string(body, "message");
            std::string data = json_get_string(body, "data");

            // Format: [Browser] level: message
            const char* prefix = "[Browser]";
            if (level == "error") {
                fprintf(stderr, "\033[31m%s ERROR: %s%s%s\033[0m\n", prefix, msg.c_str(),
                        data.empty() ? "" : " | ", data.c_str());
            } else if (level == "warn") {
                fprintf(stderr, "\033[33m%s WARN: %s%s%s\033[0m\n", prefix, msg.c_str(),
                        data.empty() ? "" : " | ", data.c_str());
            } else {
                fprintf(stderr, "%s %s: %s%s%s\n", prefix, level.c_str(), msg.c_str(),
                        data.empty() ? "" : " | ", data.c_str());
            }

            send_json_response(fd, "{\"ok\": true}");
            return;
        }

        if (path == "/api/error" && method == "POST") {
            // Client error reporting endpoint - capture JavaScript errors, exceptions, and crashes
            std::string message = json_get_string(body, "message");
            std::string stack = json_get_string(body, "stack");
            std::string url = json_get_string(body, "url");
            std::string line = json_get_string(body, "line");
            std::string col = json_get_string(body, "col");
            std::string type = json_get_string(body, "type");

            // Format: [Browser ERROR] with red color for visibility
            fprintf(stderr, "\033[1;31m[Browser ERROR]\033[0m ");

            if (!type.empty()) {
                fprintf(stderr, "%s: ", type.c_str());
            }

            fprintf(stderr, "%s", message.c_str());

            if (!url.empty()) {
                fprintf(stderr, "\n  at %s", url.c_str());
                if (!line.empty()) {
                    fprintf(stderr, ":%s", line.c_str());
                    if (!col.empty()) {
                        fprintf(stderr, ":%s", col.c_str());
                    }
                }
            }

            if (!stack.empty()) {
                // Print stack trace with indentation
                fprintf(stderr, "\n  Stack trace:\n");
                // Split by newlines and indent each line
                size_t pos = 0;
                std::string stack_copy = stack;
                while ((pos = stack_copy.find('\n')) != std::string::npos) {
                    std::string line = stack_copy.substr(0, pos);
                    if (!line.empty()) {
                        fprintf(stderr, "    %s\n", line.c_str());
                    }
                    stack_copy.erase(0, pos + 1);
                }
                if (!stack_copy.empty()) {
                    fprintf(stderr, "    %s\n", stack_copy.c_str());
                }
            } else {
                fprintf(stderr, "\n");
            }

            send_json_response(fd, "{\"ok\": true}");
            return;
        }

        // Static files
        std::string content_type = "text/html";
        std::string disk_path;

        // Map paths to files and content types
        if (path == "/" || path == "/index.html") {
            disk_path = client_dir_ + "/index.html";
            content_type = "text/html";
        } else if (path == "/client.js") {
            disk_path = client_dir_ + "/client.js";
            content_type = "application/javascript";
        } else if (path == "/styles.css") {
            disk_path = client_dir_ + "/styles.css";
            content_type = "text/css";
        }

        // Read file from disk
        std::string file_content;
        if (!disk_path.empty()) {
            std::ifstream file(disk_path);
            if (file.is_open()) {
                std::stringstream buffer;
                buffer << file.rdbuf();
                file_content = buffer.str();
            }
        }

        if (!file_content.empty()) {
            size_t content_len = file_content.size();

            std::string response = "HTTP/1.1 200 OK\r\n";
            response += "Content-Type: " + content_type + "\r\n";
            response += "Content-Length: " + std::to_string(content_len) + "\r\n";
            response += "Connection: close\r\n";
            response += "\r\n";
            send(fd, response.c_str(), response.size(), 0);
            send(fd, file_content.c_str(), content_len, 0);
        } else {
            std::string response = "HTTP/1.1 404 Not Found\r\n";
            response += "Content-Type: text/plain\r\n";
            response += "Content-Length: 9\r\n";
            response += "Connection: close\r\n";
            response += "\r\n";
            response += "Not Found";
            send(fd, response.c_str(), response.size(), 0);
        }
    }

    void send_json_response(int fd, const std::string& json_body) {
        std::string response = "HTTP/1.1 200 OK\r\n";
        response += "Content-Type: application/json\r\n";
        response += "Content-Length: " + std::to_string(json_body.size()) + "\r\n";
        response += "Connection: close\r\n";
        response += "\r\n";
        response += json_body;
        send(fd, response.c_str(), response.size(), 0);
    }

    int port_ = 8000;
    int server_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::string client_dir_ = "client";  // Directory for client files
};


/*
 * WebRTC Peer Connection
 */

struct PeerConnection {
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> video_track;
    std::shared_ptr<rtc::DataChannel> data_channel;
    std::string id;
    CodecType codec = CodecType::H264;  // Codec type for this peer
    bool ready = false;
    bool has_remote_description = false;
    std::vector<std::pair<std::string, std::string>> pending_candidates;  // candidate, mid
};


/*
 * WebRTC Server
 */

class WebRTCServer {
public:
    bool init(int signaling_port) {
        port_ = signaling_port;

        rtc::InitLogger(rtc::LogLevel::Error);
        rtc::Preload();

        try {
            rtc::WebSocketServer::Configuration config;
            config.port = signaling_port;
            config.enableTls = false;

            ws_server_ = std::make_unique<rtc::WebSocketServer>(config);

            ws_server_->onClient([this](std::shared_ptr<rtc::WebSocket> ws) {
                ws->onOpen([ws]() {
                    std::string welcome = "{\"type\":\"welcome\",\"peerId\":\"server\"}";
                    ws->send(welcome);
                });

                ws->onMessage([this, ws](auto data) {
                    if (std::holds_alternative<std::string>(data)) {
                        process_signaling(ws, std::get<std::string>(data));
                    }
                });

                ws->onError([](std::string error) {
                    fprintf(stderr, "[WebRTC] WebSocket error: %s\n", error.c_str());
                });

                ws->onClosed([this, ws]() {
                    std::lock_guard<std::mutex> lock(peers_mutex_);
                    auto it = ws_to_peer_id_.find(ws.get());
                    if (it != ws_to_peer_id_.end()) {
                        peers_.erase(it->second);
                        ws_to_peer_id_.erase(it);
                        peer_count_--;
                        fprintf(stderr, "[WebRTC] Peer disconnected, %d remaining\n", peer_count_.load());
                    }
                });
            });

            initialized_ = true;
            fprintf(stderr, "WebRTC: Signaling server on port %d\n", signaling_port);

        } catch (const std::exception& e) {
            fprintf(stderr, "WebRTC: Failed to start server: %s\n", e.what());
            return false;
        }

        return true;
    }

    void shutdown() {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        peers_.clear();
        ws_to_peer_id_.clear();
        ws_server_.reset();
        initialized_ = false;
    }

    // Send H.264 frame via RTP video track
    void send_h264_frame(const std::vector<uint8_t>& data, bool is_keyframe) {
        if (data.empty() || peer_count_ == 0) return;

        std::lock_guard<std::mutex> lock(peers_mutex_);

        // Calculate frame timestamp using chrono for precise timing
        // H264 clock rate is 90000 Hz
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<double>(now - start_time_);

        // Use the sendFrame method with FrameInfo for proper RTP timestamps
        rtc::FrameInfo frameInfo(elapsed);

        // Log only IDR frame sends (P frames logged in stats summary)
        if (is_keyframe) {
            fprintf(stderr, "[WebRTC] Sending IDR frame: %zu bytes\n", data.size());
        }

        for (auto& [id, peer] : peers_) {
            if (peer->codec != CodecType::H264) continue;  // Skip non-H264 peers
            if (peer->ready && peer->video_track && peer->video_track->isOpen()) {
                try {
                    // sendFrame with FrameInfo provides proper RTP timestamps
                    peer->video_track->sendFrame(
                        reinterpret_cast<const std::byte*>(data.data()),
                        data.size(),
                        frameInfo);
                } catch (const std::exception& e) {
                    fprintf(stderr, "[WebRTC] Send error to %s: %s\n", id.c_str(), e.what());
                }
            }
        }
    }

    // Send PNG frame via DataChannel (binary) with metadata header
    // Frame format: [8-byte t1_frame_ready] [4-byte x] [4-byte y] [4-byte width] [4-byte height]
    //               [4-byte frame_width] [4-byte frame_height] [8-byte t4_send_time]
    //               [4-byte ping_seq] [8-byte ping_t1] [8-byte ping_t2] [8-byte ping_t3]
    //               [8-byte ping_t4] [8-byte ping_t5] [PNG data]
    //   All values are little-endian uint32/uint64
    //   t1_frame_ready = emulator frame completion time (from SHM)
    //   t4_send_time = server send time (captured here)
    //   x, y, width, height = dirty rect position and size
    //   frame_width, frame_height = full screen resolution
    //   ping_* = complete ping roundtrip with timestamps at each layer (0 if no ping)
    void send_png_frame(const std::vector<uint8_t>& data, uint64_t t1_frame_ready_ms,
                        uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                        uint32_t frame_width, uint32_t frame_height) {
        if (data.empty() || peer_count_ == 0) return;

        // Sanity check: PNG data shouldn't be larger than 10MB for 1920x1080
        if (data.size() > 10 * 1024 * 1024) {
            fprintf(stderr, "[WebRTC] ERROR: PNG data size %zu is too large, skipping frame\n", data.size());
            return;
        }

        // T4: Capture timestamp right before sending (Unix epoch milliseconds)
        // Use time() * 1000 + microseconds to get accurate Unix epoch timestamp
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t t4_send_ms = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

        // Read ping echo from SHM (emulator stores last received ping with all timestamps)
        // OPTIMIZED: Only ping_sequence is atomic - acts as "ready" flag
        // Read-acquire on ping_sequence ensures all timestamp writes are visible
        uint32_t ping_seq = 0;
        uint64_t ping_t1_browser_ms = 0;
        uint64_t ping_t2_server_us = 0;
        uint64_t ping_t3_emulator_us = 0;
        uint64_t ping_t4_frame_ready_us = 0;
        if (g_video_shm) {
            // Atomic read-acquire: ensures visibility of all previous writes to ping_timestamps
            ping_seq = ATOMIC_LOAD(g_video_shm->ping_sequence);

            // If ping available, read timestamp struct (no atomics needed - seq acts as guard)
            if (ping_seq > 0) {
                ping_t1_browser_ms = g_video_shm->ping_timestamps.t1_browser_ms;
                ping_t2_server_us = g_video_shm->ping_timestamps.t2_server_us;
                ping_t3_emulator_us = g_video_shm->ping_timestamps.t3_emulator_us;
                ping_t4_frame_ready_us = g_video_shm->ping_timestamps.t4_frame_us;
                // Note: Ping echo logging happens in browser when it receives the echo
            }
        }
        // t5 is server send time (same as t4_send_ms from frame metadata)
        uint64_t ping_t5_server_send_us = t4_send_ms * 1000;  // Convert ms to us

        // Build frame with metadata header (84 bytes total: 40 base + 44 ping)
        std::vector<uint8_t> frame_with_header;
        try {
            frame_with_header.resize(84 + data.size());
        } catch (const std::bad_alloc& e) {
            fprintf(stderr, "[WebRTC] ERROR: Failed to allocate %zu bytes for frame header\n", 84 + data.size());
            return;
        }

        // T1: 8-byte emulator frame ready time (from SHM)
        for (int i = 0; i < 8; i++) {
            frame_with_header[i] = (t1_frame_ready_ms >> (i * 8)) & 0xFF;
        }
        // 4-byte x coordinate (dirty rect)
        for (int i = 0; i < 4; i++) {
            frame_with_header[8 + i] = (x >> (i * 8)) & 0xFF;
        }
        // 4-byte y coordinate (dirty rect)
        for (int i = 0; i < 4; i++) {
            frame_with_header[12 + i] = (y >> (i * 8)) & 0xFF;
        }
        // 4-byte width (dirty rect)
        for (int i = 0; i < 4; i++) {
            frame_with_header[16 + i] = (width >> (i * 8)) & 0xFF;
        }
        // 4-byte height (dirty rect)
        for (int i = 0; i < 4; i++) {
            frame_with_header[20 + i] = (height >> (i * 8)) & 0xFF;
        }
        // 4-byte frame width (full resolution)
        for (int i = 0; i < 4; i++) {
            frame_with_header[24 + i] = (frame_width >> (i * 8)) & 0xFF;
        }
        // 4-byte frame height (full resolution)
        for (int i = 0; i < 4; i++) {
            frame_with_header[28 + i] = (frame_height >> (i * 8)) & 0xFF;
        }
        // T4: 8-byte server send time
        for (int i = 0; i < 8; i++) {
            frame_with_header[32 + i] = (t4_send_ms >> (i * 8)) & 0xFF;
        }
        // Ping echo with all timestamps (44 bytes: sequence + 5 timestamps)
        // 4-byte ping sequence number (0 if no ping received)
        for (int i = 0; i < 4; i++) {
            frame_with_header[40 + i] = (ping_seq >> (i * 8)) & 0xFF;
        }
        // 8-byte t1: browser send time (performance.now() milliseconds)
        for (int i = 0; i < 8; i++) {
            frame_with_header[44 + i] = (ping_t1_browser_ms >> (i * 8)) & 0xFF;
        }
        // 8-byte t2: server receive time (CLOCK_REALTIME microseconds)
        for (int i = 0; i < 8; i++) {
            frame_with_header[52 + i] = (ping_t2_server_us >> (i * 8)) & 0xFF;
        }
        // 8-byte t3: emulator receive time (CLOCK_REALTIME microseconds)
        for (int i = 0; i < 8; i++) {
            frame_with_header[60 + i] = (ping_t3_emulator_us >> (i * 8)) & 0xFF;
        }
        // 8-byte t4: emulator/frame ready time (CLOCK_REALTIME microseconds)
        for (int i = 0; i < 8; i++) {
            frame_with_header[68 + i] = (ping_t4_frame_ready_us >> (i * 8)) & 0xFF;
        }
        // 8-byte t5: server send time (CLOCK_REALTIME microseconds)
        for (int i = 0; i < 8; i++) {
            frame_with_header[76 + i] = (ping_t5_server_send_us >> (i * 8)) & 0xFF;
        }
        // Copy PNG data after 84-byte header
        memcpy(frame_with_header.data() + 84, data.data(), data.size());

        std::lock_guard<std::mutex> lock(peers_mutex_);

        for (auto& [id, peer] : peers_) {
            if (peer->codec != CodecType::PNG) continue;  // Skip non-PNG peers
            if (peer->ready && peer->data_channel && peer->data_channel->isOpen()) {
                try {
                    peer->data_channel->send(
                        reinterpret_cast<const std::byte*>(frame_with_header.data()),
                        frame_with_header.size());
                } catch (const std::exception& e) {
                    fprintf(stderr, "[WebRTC] PNG send error to %s: %s\n", id.c_str(), e.what());
                }
            }
        }
    }

    // Check if any peer uses a specific codec
    bool has_codec_peer(CodecType codec) {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        for (auto& [id, peer] : peers_) {
            if (peer->codec == codec && peer->ready) return true;
        }
        return false;
    }

    // Get codec peer counts in one lock (more efficient for frame loop)
    struct CodecPeerCounts {
        int h264 = 0;
        int png = 0;
        int raw = 0;
    };

    CodecPeerCounts get_codec_peer_counts() {
        CodecPeerCounts counts;
        std::lock_guard<std::mutex> lock(peers_mutex_);
        for (auto& [id, peer] : peers_) {
            if (!peer->ready) continue;
            switch (peer->codec) {
                case CodecType::H264: counts.h264++; break;
                case CodecType::PNG: counts.png++; break;
                case CodecType::RAW: counts.raw++; break;
            }
        }
        return counts;
    }

    int peer_count() { return peer_count_.load(); }
    bool is_enabled() { return initialized_.load(); }

    // Disconnect all peers (e.g., when emulator restarts and resolution changes)
    void disconnect_all_peers() {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        fprintf(stderr, "[WebRTC] Disconnecting all peers (%d) due to emulator restart\n", (int)peers_.size());
        // Close all peer connections - this will trigger browser reconnections
        peers_.clear();
        ws_to_peer_id_.clear();
        peer_count_.store(0);
    }

private:
    void process_signaling(std::shared_ptr<rtc::WebSocket> ws, const std::string& msg) {
        std::string type = json_get_string(msg, "type");

        if (type == "connect") {
            std::string peer_id = "peer_" + std::to_string(rand());
            auto peer = std::make_shared<PeerConnection>();
            peer->id = peer_id;

            // Use server-side codec preference (from prefs file)
            peer->codec = g_server_codec;
            const char* codec_name = (g_server_codec == CodecType::H264) ? "h264" :
                                     (g_server_codec == CodecType::PNG) ? "png" : "raw";
            if (g_debug_connection) {
                fprintf(stderr, "[WebRTC] Peer %s using %s codec (server-configured)\n",
                        peer_id.c_str(), codec_name);
            }

            // Send acknowledgment with codec info so client knows what to expect
            std::string ack = "{\"type\":\"connected\",\"peer_id\":\"" + peer_id +
                              "\",\"codec\":\"" + codec_name + "\"}";
            ws->send(ack);

            rtc::Configuration config;
            config.iceServers.emplace_back("stun:stun.l.google.com:19302");
            // Allow large video frames (up to 16MB for high-res dithered content)
            config.maxMessageSize = 16 * 1024 * 1024;

            peer->pc = std::make_shared<rtc::PeerConnection>(config);

            {
                std::lock_guard<std::mutex> lock(peers_mutex_);
                ws_to_peer_id_[ws.get()] = peer_id;
                peers_[peer_id] = peer;
                peer_count_++;
            }

            std::weak_ptr<rtc::PeerConnection> wpc = peer->pc;
            peer->pc->onGatheringStateChange([ws, peer_id, wpc](rtc::PeerConnection::GatheringState state) {
                if (state == rtc::PeerConnection::GatheringState::Complete) {
                    if (auto pc = wpc.lock()) {
                        auto description = pc->localDescription();
                        if (description) {
                            std::string sdp = std::string(description.value());
                            std::string type_str = description->typeString();
                            std::string response = "{\"type\":\"" + type_str + "\",\"sdp\":\"" + json_escape(sdp) + "\"}";
                            ws->send(response);
                        }
                    }
                }
            });

            // Add video track with H.264 codec
            // Use profile-level-id=42e01f (Constrained Baseline, Level 3.1) in SDP
            // This matches what browsers advertise for WebRTC H.264 support.
            // Browsers can still decode higher levels, they just don't advertise it.
            // level-asymmetry-allowed=1 means we can send higher levels than advertised.
            rtc::Description::Video media("video-stream", rtc::Description::Direction::SendOnly);
            media.addH264Codec(96, "profile-level-id=42e01f;packetization-mode=1;level-asymmetry-allowed=1");
            media.addSSRC(ssrc_, "video-stream", "stream1", "video-stream");
            peer->video_track = peer->pc->addTrack(media);

            // Set up H.264 RTP packetizer (handles NAL unit fragmentation)
            auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
                ssrc_, "video-stream", 96, rtc::H264RtpPacketizer::ClockRate
            );
            auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
                rtc::H264RtpPacketizer::Separator::LongStartSequence,
                rtpConfig
            );
            peer->video_track->setMediaHandler(packetizer);

            peer->video_track->onOpen([peer]() {
                if (g_debug_connection) {
                    fprintf(stderr, "[WebRTC] Video track OPEN for %s - ready to send frames!\n", peer->id.c_str());
                }
                peer->ready = true;
                // Request keyframe so new peer gets a complete picture
                g_request_keyframe.store(true);
            });

            peer->video_track->onClosed([peer]() {
                fprintf(stderr, "[WebRTC] Video track CLOSED for %s\n", peer->id.c_str());
                peer->ready = false;
            });

            peer->video_track->onError([peer](std::string error) {
                fprintf(stderr, "[WebRTC] Video track ERROR for %s: %s\n", peer->id.c_str(), error.c_str());
            });

            peer->pc->onStateChange([peer](rtc::PeerConnection::State state) {
                const char* state_str = "unknown";
                switch (state) {
                    case rtc::PeerConnection::State::New: state_str = "New"; break;
                    case rtc::PeerConnection::State::Connecting: state_str = "Connecting"; break;
                    case rtc::PeerConnection::State::Connected: state_str = "Connected"; break;
                    case rtc::PeerConnection::State::Disconnected: state_str = "Disconnected"; break;
                    case rtc::PeerConnection::State::Failed: state_str = "Failed"; break;
                    case rtc::PeerConnection::State::Closed: state_str = "Closed"; break;
                }
                if (g_debug_connection) {
                    fprintf(stderr, "[WebRTC] Peer %s state: %s\n", peer->id.c_str(), state_str);
                }
            });

            peer->pc->onIceStateChange([peer](rtc::PeerConnection::IceState state) {
                const char* state_str = "unknown";
                switch (state) {
                    case rtc::PeerConnection::IceState::New: state_str = "New"; break;
                    case rtc::PeerConnection::IceState::Checking: state_str = "Checking"; break;
                    case rtc::PeerConnection::IceState::Connected: state_str = "Connected"; break;
                    case rtc::PeerConnection::IceState::Completed: state_str = "Completed"; break;
                    case rtc::PeerConnection::IceState::Disconnected: state_str = "Disconnected"; break;
                    case rtc::PeerConnection::IceState::Failed: state_str = "Failed"; break;
                    case rtc::PeerConnection::IceState::Closed: state_str = "Closed"; break;
                }
                if (g_debug_connection) {
                    fprintf(stderr, "[WebRTC] Peer %s ICE state: %s\n", peer->id.c_str(), state_str);
                }
            });

            // Add data channel for input
            peer->data_channel = peer->pc->createDataChannel("input");
            peer->data_channel->onOpen([peer_id = peer->id]() {
                if (g_debug_connection) {
                    fprintf(stderr, "[WebRTC] DataChannel OPEN for %s\n", peer_id.c_str());
                }
            });
            peer->data_channel->onMessage([this](auto data) {
                if (std::holds_alternative<std::string>(data)) {
                    const std::string& msg = std::get<std::string>(data);
                    if (g_debug_connection) {
                        static int msg_count = 0;
                        if (msg_count++ < 5) {
                            fprintf(stderr, "[WebRTC] DataChannel message: '%s'\n", msg.c_str());
                        }
                    }
                    handle_input(msg);
                } else if (g_debug_connection) {
                    fprintf(stderr, "[WebRTC] DataChannel received binary data\n");
                }
            });

            // Also handle incoming data channels from browser
            peer->pc->onDataChannel([this, peer_id = peer->id](std::shared_ptr<rtc::DataChannel> dc) {
                fprintf(stderr, "[WebRTC] Incoming DataChannel '%s' from %s\n",
                        dc->label().c_str(), peer_id.c_str());
                dc->onOpen([label = dc->label(), peer_id]() {
                    fprintf(stderr, "[WebRTC] Incoming DataChannel '%s' OPEN for %s\n",
                            label.c_str(), peer_id.c_str());
                });
                dc->onMessage([this](auto data) {
                    if (std::holds_alternative<std::string>(data)) {
                        handle_input(std::get<std::string>(data));
                    }
                });
            });

            peer->pc->setLocalDescription();
        }
        else if (type == "answer") {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            auto it = ws_to_peer_id_.find(ws.get());
            if (it != ws_to_peer_id_.end()) {
                auto peer = peers_[it->second];
                std::string sdp = json_get_string(msg, "sdp");
                if (g_debug_connection) {
                    fprintf(stderr, "[WebRTC] Received answer from %s (sdp length=%zu)\n",
                            peer->id.c_str(), sdp.size());
                }

                // Set remote description
                try {
                    peer->pc->setRemoteDescription(rtc::Description(sdp, "answer"));
                    peer->has_remote_description = true;
                    if (g_debug_connection) {
                        fprintf(stderr, "[WebRTC] Remote description set for %s\n", peer->id.c_str());
                    }
                } catch (const std::exception& e) {
                    fprintf(stderr, "[WebRTC] ERROR setting remote description for %s: %s\n",
                            peer->id.c_str(), e.what());
                    return;
                }

                // Now add any pending ICE candidates
                if (!peer->pending_candidates.empty()) {
                    fprintf(stderr, "[WebRTC] Adding %zu pending ICE candidates\n",
                            peer->pending_candidates.size());
                    for (const auto& [candidate, mid] : peer->pending_candidates) {
                        try {
                            peer->pc->addRemoteCandidate(rtc::Candidate(candidate, mid));
                            fprintf(stderr, "[WebRTC] Added pending candidate: %s\n", mid.c_str());
                        } catch (const std::exception& e) {
                            fprintf(stderr, "[WebRTC] Failed to add pending candidate: %s\n", e.what());
                        }
                    }
                    peer->pending_candidates.clear();
                }
            }
        }
        else if (type == "candidate") {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            auto it = ws_to_peer_id_.find(ws.get());
            if (it != ws_to_peer_id_.end()) {
                auto peer = peers_[it->second];
                std::string candidate = json_get_string(msg, "candidate");
                std::string mid = json_get_string(msg, "mid");
                if (!candidate.empty()) {
                    if (peer->has_remote_description) {
                        // Remote description is set, add candidate immediately
                        fprintf(stderr, "[WebRTC] Adding ICE candidate from %s (mid=%s)\n",
                                peer->id.c_str(), mid.c_str());
                        try {
                            peer->pc->addRemoteCandidate(rtc::Candidate(candidate, mid));
                        } catch (const std::exception& e) {
                            fprintf(stderr, "[WebRTC] Failed to add candidate: %s\n", e.what());
                        }
                    } else {
                        // Queue candidate - remote description not set yet
                        fprintf(stderr, "[WebRTC] Queuing ICE candidate from %s (mid=%s)\n",
                                peer->id.c_str(), mid.c_str());
                        peer->pending_candidates.emplace_back(candidate, mid);
                    }
                }
            }
        }
    }

    void handle_input(const std::string& msg) {
        // Simple text protocol from browser: M dx,dy | D btn | U btn | K code | k code
        // Server converts browser keycodes to Mac keycodes and sends binary to emulator
        if (msg.empty()) return;

        char cmd = msg[0];
        const char* args = msg.c_str() + 1;

        static uint8_t current_buttons = 0;

        switch (cmd) {
            case 'M': {
                // Mouse move: M dx,dy,timestamp
                int dx = 0, dy = 0;
                double ts = 0;
                if (sscanf(args, "%d,%d,%lf", &dx, &dy, &ts) >= 2) {
                    uint64_t browser_ts = static_cast<uint64_t>(ts);
                    send_mouse_input(dx, dy, current_buttons, browser_ts);
                    g_mouse_move_count++;
                }
                break;
            }
            case 'D': {
                // Mouse down: D button,timestamp
                int button = 0;
                double ts = 0;
                sscanf(args, "%d,%lf", &button, &ts);
                if (button == 0) current_buttons |= MACEMU_MOUSE_LEFT;
                else if (button == 1) current_buttons |= MACEMU_MOUSE_MIDDLE;
                else if (button == 2) current_buttons |= MACEMU_MOUSE_RIGHT;
                uint64_t browser_ts = static_cast<uint64_t>(ts);
                send_mouse_input(0, 0, current_buttons, browser_ts);
                g_mouse_click_count++;
                break;
            }
            case 'U': {
                // Mouse up: U button,timestamp
                int button = 0;
                double ts = 0;
                sscanf(args, "%d,%lf", &button, &ts);
                if (button == 0) current_buttons &= ~MACEMU_MOUSE_LEFT;
                else if (button == 1) current_buttons &= ~MACEMU_MOUSE_MIDDLE;
                else if (button == 2) current_buttons &= ~MACEMU_MOUSE_RIGHT;
                uint64_t browser_ts = static_cast<uint64_t>(ts);
                send_mouse_input(0, 0, current_buttons, browser_ts);
                g_mouse_click_count++;
                break;
            }
            case 'K': {
                // Key down: K keycode
                int keycode = atoi(args);
                int mac_code = browser_to_mac_keycode(keycode);
                if (mac_code >= 0) {
                    send_key_input(mac_code, true);
                    g_key_count++;
                }
                break;
            }
            case 'k': {
                // Key up: k keycode
                int keycode = atoi(args);
                int mac_code = browser_to_mac_keycode(keycode);
                if (mac_code >= 0) {
                    send_key_input(mac_code, false);
                    g_key_count++;
                }
                break;
            }
            case 'P': {
                // Ping: P sequence,timestamp
                // NOTE: Ping responses are only sent in PNG/RAW codec mode via DataChannel metadata header.
                // H.264 uses RTP video track with no metadata support - ping echoes would be discarded.
                uint32_t sequence = 0;
                double ts = 0;
                if (sscanf(args, "%u,%lf", &sequence, &ts) == 2) {
                    uint64_t timestamp_ms = static_cast<uint64_t>(ts);
                    bool success = send_ping_input(sequence, timestamp_ms);
                    if (g_debug_perf) {
                        fprintf(stderr, "[Server] Ping #%u (t1=%.1fms) forwarded to emulator: %s\n",
                                sequence, ts, success ? "OK" : "FAILED");
                    }
                } else {
                    fprintf(stderr, "[Server] ERROR: Failed to parse ping message: '%s'\n", args);
                }
                break;
            }
        }
    }

    std::atomic<bool> initialized_{false};
    std::atomic<int> peer_count_{0};

    int port_ = 8090;
    std::unique_ptr<rtc::WebSocketServer> ws_server_;
    std::chrono::steady_clock::time_point start_time_ = std::chrono::steady_clock::now();

    std::mutex peers_mutex_;
    std::map<std::string, std::shared_ptr<PeerConnection>> peers_;
    std::map<rtc::WebSocket*, std::string> ws_to_peer_id_;

    uint32_t ssrc_ = 1;
};

// Implementation of disconnect_from_emulator (needs WebRTCServer definition)
static void disconnect_from_emulator(WebRTCServer* webrtc) {
    disconnect_control_socket();
    disconnect_video_shm();
    g_emulator_pid = -1;

    // Disconnect all WebRTC peers so they reconnect with fresh encoder/decoder instances
    // This is important when emulator restarts with different resolution
    if (webrtc) {
        webrtc->disconnect_all_peers();
    }
}

/*
 * Main video processing loop
 */

static void video_loop(WebRTCServer& webrtc, H264Encoder& h264_encoder, PNGEncoder& png_encoder) {
    auto last_stats_time = std::chrono::steady_clock::now();
    auto last_emu_check = std::chrono::steady_clock::now();
    auto last_scan_time = std::chrono::steady_clock::now();
    int frames_encoded = 0;

    // Track input counts between stats intervals
    uint64_t last_mouse_move = 0;

    // Create epoll instance for low-latency event notification (REQUIRED)
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        fprintf(stderr, "Video: FATAL: Failed to create epoll: %s\n", strerror(errno));
        fprintf(stderr, "Video: epoll is required for frame synchronization (no polling fallback)\n");
        return;
    }
    int current_eventfd = -1;  // Track which eventfd is registered

    fprintf(stderr, "Video: Starting frame processing loop\n");

    while (g_running) {
        auto now = std::chrono::steady_clock::now();

        // Periodically scan for emulators if not connected
        if (!g_emulator_connected) {
            auto scan_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_scan_time);
            if (scan_elapsed.count() >= 500) {
                last_scan_time = now;

                // If target PID specified, try that
                if (g_target_emulator_pid > 0) {
                    if (try_connect_to_emulator(g_target_emulator_pid)) {
                        fprintf(stderr, "Video: Connected to emulator PID %d\n", g_target_emulator_pid);
                    }
                } else {
                    // Scan for any running emulator
                    auto pids = scan_for_emulators();
                    for (pid_t pid : pids) {
                        if (try_connect_to_emulator(pid)) {
                            fprintf(stderr, "Video: Found and connected to emulator PID %d\n", pid);
                            break;
                        }
                    }
                }
            }
        }

        // Check if we started an emulator and it exited
        auto emu_check_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_emu_check);
        if (emu_check_elapsed.count() >= 500) {
            last_emu_check = now;

            int exit_code = check_emulator_status();
            if (exit_code > 0 && g_auto_start_emulator) {
                // Emulator we started exited - check if restart requested (exit code 75)
                if (exit_code == 75) {
                    fprintf(stderr, "Video: Auto-restarting emulator...\n");
                    disconnect_from_emulator(&webrtc);
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    start_emulator();
                }
            }

            // Handle restart request from web UI
            if (g_restart_emulator_requested.exchange(false)) {
                fprintf(stderr, "Video: Restart requested from web UI\n");
                if (g_started_emulator_pid > 0) {
                    stop_emulator();
                } else {
                    send_command(MACEMU_CMD_RESET);
                }
                disconnect_from_emulator(&webrtc);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                if (g_auto_start_emulator) {
                    start_emulator();
                }
            }
        }

        // Check if emulator disconnected
        if (g_emulator_connected && g_control_socket >= 0) {
            char buf;
            ssize_t n = recv(g_control_socket, &buf, 1, MSG_PEEK | MSG_DONTWAIT);
            if (n == 0) {
                // Connection closed
                fprintf(stderr, "Video: Emulator disconnected\n");
                disconnect_from_emulator(&webrtc);
            }
        }

        // Wait for frames
        if (!g_video_shm) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Register eventfd with epoll if we just connected to emulator
        if (g_frame_ready_eventfd >= 0 && current_eventfd != g_frame_ready_eventfd) {
            // Remove old eventfd if any
            if (current_eventfd >= 0) {
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, current_eventfd, nullptr);
            }

            // Add new eventfd
            struct epoll_event ev;
            ev.events = EPOLLIN;
            ev.data.fd = g_frame_ready_eventfd;
            if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, g_frame_ready_eventfd, &ev) == 0) {
                current_eventfd = g_frame_ready_eventfd;
                fprintf(stderr, "Video: Registered eventfd %d with epoll\n", current_eventfd);
            } else {
                fprintf(stderr, "Video: FATAL: Failed to add eventfd %d to epoll: %s\n",
                        g_frame_ready_eventfd, strerror(errno));
                // This is fatal - can't proceed without eventfd
                disconnect_from_emulator(&webrtc);
                continue;
            }
        }

        // Wait for new frame via epoll (blocking on eventfd)
        if (current_eventfd < 0) {
            // No eventfd registered yet, wait for emulator connection
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        struct epoll_event events[1];
        // Wait up to 5ms for frame ready event
        int n = epoll_wait(epoll_fd, events, 1, 5);
        if (n > 0) {
            // Frame ready! Consume the eventfd value (required for semaphore mode)
            // This read() provides memory barrier - all emulator writes before write(eventfd) are now visible
            uint64_t val;
            ssize_t ret = read(current_eventfd, &val, sizeof(val));
            (void)ret;  // Suppress unused warning
        } else {
            // Timeout or error - continue loop
            continue;
        }

        // Latency measurement: time from emulator frame completion to now
        // Plain read - synchronized by eventfd read above
        uint64_t frame_timestamp_us = g_video_shm->timestamp_us;

        // Use CLOCK_REALTIME to match the emulator's timestamp (both in same clock domain)
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t server_now_us = (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;

        // Read frame dimensions (plain reads - synchronized by eventfd)
        uint32_t width = g_video_shm->width;
        uint32_t height = g_video_shm->height;

        if (width == 0 || height == 0 || width > MACEMU_MAX_WIDTH || height > MACEMU_MAX_HEIGHT) {
            continue;
        }

        // Get BGRA frame from ready buffer
        // Emulator always outputs BGRA (B,G,R,A bytes), which is libyuv "ARGB"
        uint8_t* frame_data = macemu_get_ready_bgra(g_video_shm);
        int stride = macemu_get_bgra_stride();

        // Check if keyframe requested (new peer connected)
        bool keyframe_requested = g_request_keyframe.exchange(false);
        if (keyframe_requested) {
            h264_encoder.request_keyframe();
        }

        // Encode and send to H.264 peers
        auto encode_start = std::chrono::steady_clock::now();
        if (webrtc.has_codec_peer(CodecType::H264)) {
            EncodedFrame frame = h264_encoder.encode_bgra(frame_data, width, height, stride);
            if (!frame.data.empty()) {
                webrtc.send_h264_frame(frame.data, frame.is_keyframe);
                frames_encoded++;
            }
        }

        // Encode and send to PNG peers using dirty rects from emulator
        if (webrtc.has_codec_peer(CodecType::PNG)) {
            // Read dirty rect from SHM (plain reads - synchronized by eventfd)
            uint32_t dirty_x = g_video_shm->dirty_x;
            uint32_t dirty_y = g_video_shm->dirty_y;
            uint32_t dirty_width = g_video_shm->dirty_width;
            uint32_t dirty_height = g_video_shm->dirty_height;

            // Force full frame if keyframe requested (new peer connected)
            if (keyframe_requested) {
                dirty_x = 0;
                dirty_y = 0;
                dirty_width = width;
                dirty_height = height;
            }

            // Heartbeat mechanism: Send tiny frame to carry ping echoes even when screen is static
            // This prevents ping responses from being delayed indefinitely on idle screens
            static auto last_heartbeat = std::chrono::steady_clock::now();
            if (dirty_width == 0 || dirty_height == 0) {
                // Check if there's a pending ping echo
                uint32_t ping_seq = ATOMIC_LOAD(g_video_shm->ping_sequence);
                if (ping_seq > 0) {
                    auto heartbeat_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_heartbeat);
                    // Send heartbeat at most once per second to avoid spam
                    if (heartbeat_elapsed.count() >= 1000) {
                        // Send a tiny 1x1 pixel frame just to carry the ping echo
                        dirty_x = 0;
                        dirty_y = 0;
                        dirty_width = 1;
                        dirty_height = 1;
                        last_heartbeat = now;
                        // Note: This encodes/sends 1 pixel which is ~100 bytes - negligible overhead
                    }
                }
            } else {
                // Screen is updating, reset heartbeat timer
                last_heartbeat = now;
            }

            // Only encode if there are changes (dirty_width > 0) or heartbeat triggered
            // Ping echoes are carried in the frame metadata header of any frame being sent
            if (dirty_width > 0 && dirty_height > 0) {
                EncodedFrame frame;

                // Check if this is a full frame or dirty rect
                if (dirty_x == 0 && dirty_y == 0 && dirty_width == width && dirty_height == height) {
                    // Full frame
                    frame = png_encoder.encode_bgra(frame_data, width, height, stride);
                } else {
                    // Dirty rectangle only
                    frame = png_encoder.encode_bgra_rect(frame_data, width, height, stride,
                                                         dirty_x, dirty_y, dirty_width, dirty_height);
                }

                if (!frame.data.empty()) {
                    // T1: Frame ready time from emulator (convert from microseconds to milliseconds)
                    uint64_t t1_frame_ready_ms = frame_timestamp_us / 1000;

                    // Send PNG with dirty rect metadata, full frame resolution, and timestamps
                    webrtc.send_png_frame(frame.data, t1_frame_ready_ms,
                                          dirty_x, dirty_y, dirty_width, dirty_height,
                                          width, height);
                    frames_encoded++;
                }
            }
            // else: no changes (dirty_width == 0), skip encoding
        }
        auto encode_end = std::chrono::steady_clock::now();

        // Track latency stats
        static uint64_t total_shm_latency_us = 0;
        static uint64_t total_encode_latency_us = 0;
        static int latency_samples = 0;

        if (frame_timestamp_us > 0) {
            uint64_t shm_latency = server_now_us - frame_timestamp_us;
            uint64_t encode_latency = std::chrono::duration_cast<std::chrono::microseconds>(
                encode_end - encode_start).count();

            total_shm_latency_us += shm_latency;
            total_encode_latency_us += encode_latency;
            latency_samples++;
        }

        // Print stats every 3 seconds
        auto stats_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_stats_time);
        if (stats_elapsed.count() >= 3000) {
            float fps = frames_encoded * 1000.0f / stats_elapsed.count();

            // Calculate input rates
            uint64_t cur_mouse_move = g_mouse_move_count.load();
            uint64_t mouse_moves = cur_mouse_move - last_mouse_move;
            float mouse_rate = mouse_moves * 1000.0f / stats_elapsed.count();

            // Calculate average video latencies (server-side only)
            float avg_shm_ms = latency_samples > 0 ? (total_shm_latency_us / latency_samples) / 1000.0f : 0;
            float avg_encode_ms = latency_samples > 0 ? (total_encode_latency_us / latency_samples) / 1000.0f : 0;

            if (g_debug_perf) {
                fprintf(stderr, "[Server] fps=%.1f peers=%d | video: shm=%.1fms enc=%.1fms | mouse: rate=%.0f/s | emu=%s pid=%d\n",
                        fps, webrtc.peer_count(),
                        avg_shm_ms, avg_encode_ms,
                        mouse_rate,
                        g_emulator_connected ? "connected" : "scanning",
                        g_emulator_pid);
            }

            // Reset latency counters
            total_shm_latency_us = 0;
            total_encode_latency_us = 0;
            latency_samples = 0;

            // Save frame as PPM (readable image format) for debugging (disabled by default)
            if (g_debug_frames) {
                static int frame_save_count = 0;
                if (frame_save_count < 3) {  // Only save first 3 frames
                    char filename[64];
                    snprintf(filename, sizeof(filename), "frame_%d_%dx%d.ppm", frame_save_count, width, height);
                    FILE* f = fopen(filename, "wb");
                    if (f) {
                        // Convert BGRA frame to RGB and save as PPM
                        // BGRA = bytes B,G,R,A (libyuv "ARGB")
                        fprintf(f, "P6\n%d %d\n255\n", width, height);
                        for (uint32_t row = 0; row < height; row++) {
                            for (uint32_t col = 0; col < width; col++) {
                                const uint8_t* pixel = frame_data + row * stride + col * 4;
                                uint8_t B = pixel[0];
                                uint8_t G = pixel[1];
                                uint8_t R = pixel[2];
                                uint8_t rgb[3] = {R, G, B};
                                fwrite(rgb, 1, 3, f);
                            }
                        }
                        fclose(f);
                        fprintf(stderr, "[Server] Saved debug frame: %s (BGRA)\n", filename);
                        frame_save_count++;
                    }
                }
            }

            last_mouse_move = cur_mouse_move;
            frames_encoded = 0;
            last_stats_time = now;
        }
    }

    // Clean up epoll
    if (epoll_fd >= 0) {
        close(epoll_fd);
    }

    fprintf(stderr, "Video: Exiting frame processing loop\n");
}


/*
 * Print usage
 */

static void print_usage(const char* program) {
    fprintf(stderr, "Usage: %s [options]\n", program);
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  -h, --help              Show this help\n");
    fprintf(stderr, "  -p, --http-port PORT    HTTP server port (default: 8000)\n");
    fprintf(stderr, "  -s, --signaling PORT    WebSocket signaling port (default: 8090)\n");
    fprintf(stderr, "  -e, --emulator PATH     Path to BasiliskII/SheepShaver executable\n");
    fprintf(stderr, "  -P, --prefs FILE        Emulator prefs file (default: basilisk_ii.prefs)\n");
    fprintf(stderr, "  -n, --no-auto-start     Don't auto-start emulator (wait for external)\n");
    fprintf(stderr, "  --pid PID               Connect to specific emulator PID\n");
    fprintf(stderr, "  --roms PATH             ROMs directory (default: storage/roms)\n");
    fprintf(stderr, "  --images PATH           Disk images directory (default: storage/images)\n");
    fprintf(stderr, "\nDebug Options:\n");
    fprintf(stderr, "  --debug-connection      Show WebRTC/ICE/signaling logs\n");
    fprintf(stderr, "  --debug-mode-switch     Show mode/resolution/color depth changes\n");
    fprintf(stderr, "  --debug-perf            Show performance stats and ping logs\n");
    fprintf(stderr, "  --debug-frames          Save frame dumps to disk (.ppm files)\n");
    fprintf(stderr, "\nArchitecture:\n");
    fprintf(stderr, "  - Emulator creates SHM at /macemu-video-{PID}\n");
    fprintf(stderr, "  - Emulator creates socket at /tmp/macemu-{PID}.sock\n");
    fprintf(stderr, "  - Server connects to emulator resources by PID\n");
    fprintf(stderr, "  - Use --pid to connect to a specific running emulator\n");
    fprintf(stderr, "\nEmulator Discovery:\n");
    fprintf(stderr, "  Server looks for ./bin/BasiliskII or ./bin/SheepShaver.\n");
    fprintf(stderr, "  Create a symlink if needed:\n");
    fprintf(stderr, "    mkdir -p bin && ln -s ../../BasiliskII/src/Unix/BasiliskII ./bin/BasiliskII\n");
}


/*
 * Main entry point
 */

int main(int argc, char* argv[]) {
    // Parse command line
    static struct option long_options[] = {
        {"help",             no_argument,       0, 'h'},
        {"http-port",        required_argument, 0, 'p'},
        {"signaling",        required_argument, 0, 's'},
        {"roms",             required_argument, 0, 'r'},
        {"images",           required_argument, 0, 'i'},
        {"emulator",         required_argument, 0, 'e'},
        {"prefs",            required_argument, 0, 'P'},
        {"no-auto-start",    no_argument,       0, 'n'},
        {"pid",              required_argument, 0, 1000},
        {"debug-connection", no_argument,       0, 1001},
        {"debug-mode-switch", no_argument,      0, 1002},
        {"debug-perf",       no_argument,       0, 1003},
        {"debug-frames",     no_argument,       0, 1004},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "hp:s:e:nP:", long_options, nullptr)) != -1) {
        switch (opt) {
            case 'h':
                print_usage(argv[0]);
                return 0;
            case 'p':
                g_http_port = atoi(optarg);
                break;
            case 's':
                g_signaling_port = atoi(optarg);
                break;
            case 'r':
                g_roms_path = optarg;
                break;
            case 'i':
                g_images_path = optarg;
                break;
            case 'e':
                g_emulator_path = optarg;
                break;
            case 'P':
                g_prefs_path = optarg;
                break;
            case 'n':
                g_auto_start_emulator = false;
                break;
            case 1000:
                g_target_emulator_pid = atoi(optarg);
                break;
            case 1001:
                g_debug_connection = true;
                break;
            case 1002:
                g_debug_mode_switch = true;
                break;
            case 1003:
                g_debug_perf = true;
                break;
            case 1004:
                g_debug_frames = true;
                break;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    // Check environment variables
    if (const char* env = getenv("BASILISK_ROMS")) g_roms_path = env;
    if (const char* env = getenv("BASILISK_IMAGES")) g_images_path = env;

    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    // Create minimal prefs file if it doesn't exist (for cold boot)
    create_minimal_prefs_if_needed();

    // Read codec preference from prefs file
    read_webcodec_pref();

    fprintf(stderr, "=== macemu WebRTC Server (v3 - emulator-owned resources) ===\n");
    fprintf(stderr, "HTTP port:      %d\n", g_http_port);
    fprintf(stderr, "Signaling port: %d\n", g_signaling_port);
    fprintf(stderr, "Prefs file:     %s\n", g_prefs_path.c_str());
    fprintf(stderr, "Video codec:    %s\n", g_server_codec == CodecType::H264 ? "H.264" : "PNG");
    fprintf(stderr, "ROMs path:      %s\n", g_roms_path.c_str());
    fprintf(stderr, "Images path:    %s\n", g_images_path.c_str());
    if (g_target_emulator_pid > 0) {
        fprintf(stderr, "Target PID:     %d\n", g_target_emulator_pid);
    }
    fprintf(stderr, "\n");

    // Start HTTP server
    HTTPServer http_server;
    if (!http_server.start(g_http_port)) {
        fprintf(stderr, "Failed to start HTTP server\n");
        return 1;
    }

    // Start WebRTC server
    WebRTCServer webrtc;
    if (!webrtc.init(g_signaling_port)) {
        fprintf(stderr, "Failed to start WebRTC server\n");
        http_server.stop();
        return 1;
    }

    fprintf(stderr, "\nOpen http://localhost:%d in your browser\n", g_http_port);

    // Create encoders
    H264Encoder h264_encoder;
    PNGEncoder png_encoder;

    // Auto-start emulator if enabled
    if (g_auto_start_emulator && g_target_emulator_pid == 0) {
        std::string emu = find_emulator();
        if (!emu.empty()) {
            fprintf(stderr, "Found emulator: %s\n", emu.c_str());
            if (start_emulator()) {
                fprintf(stderr, "Emulator started, waiting for IPC resources...\n\n");
            }
        } else {
            fprintf(stderr, "\n");
            fprintf(stderr, "ERROR: No emulator found!\n");
            fprintf(stderr, "\n");
            fprintf(stderr, "Please create a symlink in the bin/ directory:\n");
            fprintf(stderr, "  mkdir -p bin\n");
            fprintf(stderr, "  ln -s ../../BasiliskII/src/Unix/BasiliskII ./bin/BasiliskII\n");
            fprintf(stderr, "  or\n");
            fprintf(stderr, "  ln -s ../../SheepShaver/src/Unix/SheepShaver ./bin/SheepShaver\n");
            fprintf(stderr, "\n");
            fprintf(stderr, "Alternatively, use --emulator PATH to specify the executable.\n");
            fprintf(stderr, "\n");
            webrtc.shutdown();
            http_server.stop();
            return 1;
        }
    } else if (g_target_emulator_pid > 0) {
        fprintf(stderr, "Waiting to connect to emulator PID %d...\n\n", g_target_emulator_pid);
    } else {
        fprintf(stderr, "Auto-start disabled, scanning for running emulators...\n\n");
    }

    video_loop(webrtc, h264_encoder, png_encoder);

    // Stop emulator if we started it
    stop_emulator();

    // Disconnect from emulator
    disconnect_from_emulator();

    // Cleanup
    webrtc.shutdown();
    http_server.stop();

    fprintf(stderr, "Server: Shutdown complete\n");
    return 0;
}
