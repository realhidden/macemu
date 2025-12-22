# WebRTC Streaming for Basilisk II

This document describes the WebRTC streaming feature that allows running Basilisk II and accessing it through a web browser.

## Overview

Basilisk II supports browser-based access via WebRTC streaming. The implementation uses:

- **Standalone WebRTC Server** - Separate process that connects to the emulator via IPC
- **H.264 Encoding** - OpenH264 for efficient video compression
- **libdatachannel** - Lightweight C++ WebRTC library for signaling and media transport
- **libyuv** - Fast color space conversion (RGB to I420)
- **WebRTC DataChannel** - Low-latency mouse/keyboard input

## Architecture

The system uses a split architecture with IPC (Inter-Process Communication) between the emulator and the streaming server:

```
+------------------------------------------------------------------+
|                        Basilisk II                                |
|  +------------------+     +------------------------------------+  |
|  | Mac Framebuffer  |---->| video_ipc.cpp                      |  |
|  | (32/16/8/1 bpp)  |     | - RGB to I420 conversion (libyuv)  |  |
|  +------------------+     | - Triple-buffered SHM              |  |
|                           | - Unix socket for input            |  |
|                           +------------------------------------+  |
+------------------------------------------------------------------+
          |                              |
          | /macemu-video-{PID}          | /tmp/macemu-{PID}.sock
          | (Shared Memory)              | (Unix Socket)
          v                              v
+------------------------------------------------------------------+
|                    WebRTC Server (macemu-webrtc)                  |
|  +------------------+     +------------------------------------+  |
|  | I420 Reader      |---->| H.264 Encoder (OpenH264)           |  |
|  | (from SHM)       |     | - QP 48-51 for compression         |  |
|  | + Blur Filter    |     | - IDR every 2 seconds              |  |
|  +------------------+     +------------------------------------+  |
|                           +------------------------------------+  |
|                           | WebRTC (libdatachannel)            |  |
|                           | - H264RtpPacketizer                |  |
|                           | - DTLS/SRTP encryption             |  |
|                           | - ICE connectivity                 |  |
|                           +------------------------------------+  |
|                           +------------------------------------+  |
|                           | HTTP Server (port 8000)            |  |
|                           | Signaling Server (port 8090)       |  |
|                           +------------------------------------+  |
+------------------------------------------------------------------+
                                   |
                          WebRTC + DataChannel
                                   v
+------------------------------------------------------------------+
|                         Web Browser                               |
|  +------------------------+     +------------------------------+  |
|  | client.js              |---->| <video> element              |  |
|  | - WebSocket signaling  |     | - H.264 decode (browser)     |  |
|  | - RTCPeerConnection    |     | - Hardware accelerated       |  |
|  | - Input capture        |     +------------------------------+  |
|  +------------------------+                                       |
+------------------------------------------------------------------+
```

## IPC Protocol

### Shared Memory (`/macemu-video-{PID}`)

The emulator creates a POSIX shared memory segment containing:

- **Header** - Magic, version, PID, dimensions, state
- **Triple Buffer** - Three I420 frame buffers for lock-free producer/consumer
- **Atomic Indices** - Read/write buffer indices with memory barriers

```c
struct MacEmuVideoBuffer {
    uint32_t magic;           // MACEMU_VIDEO_MAGIC
    uint32_t version;         // Protocol version
    uint32_t owner_pid;       // Emulator PID
    uint32_t width, height;   // Frame dimensions
    uint32_t state;           // Connection state
    uint64_t frame_count;     // Total frames produced
    uint32_t write_index;     // Current write buffer (0-2)
    uint32_t read_index;      // Current read buffer (0-2)
    uint8_t buffers[3][...];  // Triple-buffered I420 frames
};
```

### Control Socket (`/tmp/macemu-{PID}.sock`)

Unix stream socket for input events (mouse, keyboard, commands):

```c
// Mouse input (8 bytes)
struct MacEmuMouseInput {
    uint8_t type;      // MACEMU_INPUT_MOUSE
    uint8_t flags;     // Reserved
    int16_t x, y;      // Absolute coordinates
    uint8_t buttons;   // Button state bitmask
};

// Key input (8 bytes)
struct MacEmuKeyInput {
    uint8_t type;      // MACEMU_INPUT_KEY
    uint8_t flags;     // KEY_DOWN or KEY_UP
    uint8_t mac_keycode;  // ADB keycode
    uint8_t modifiers;    // Modifier state
};
```

## Color Space Conversion

The emulator converts the Mac framebuffer to I420 (YUV 4:2:0) using libyuv:

| Mac Format | Conversion Function | Notes |
|------------|---------------------|-------|
| 32-bit ARGB | `BGRAToI420` | Big-endian ARGB = A,R,G,B in memory |
| 16-bit RGB555 | Manual + `ARGBToI420` | Byte-swap for big-endian |
| 8/4/2/1-bit | Palette lookup + `ARGBToI420` | Indexed color modes |

### Blur Filter

For 1-bit dithered modes (Mac startup screen), a 3x3 box blur is applied to the Y plane before encoding. This converts the high-frequency dither pattern into smooth gray, dramatically improving H.264 compression:

- **Without blur**: ~850 KB IDR frames (incompressible noise)
- **With blur**: ~340 KB IDR frames (60% reduction)

## H.264 Encoding

OpenH264 encoder settings optimized for low-latency streaming:

| Setting | Value | Purpose |
|---------|-------|---------|
| Usage Type | CAMERA_VIDEO_REAL_TIME | Browser-compatible H.264 |
| Rate Control | RC_OFF_MODE | Fixed QP for consistent quality |
| QP Range | 48-51 | High compression |
| IDR Interval | 2 seconds | Recovery from packet loss |
| Slice Mode | SM_SINGLE_SLICE | Simple NAL structure |
| Loop Filter | Enabled | Better compression |

Typical frame sizes at 640x480:
- **IDR frames**: ~36 KB (keyframes)
- **P frames**: ~1-5 KB (delta frames)

## Quick Start

### 1. Install Dependencies

```bash
# Ubuntu/Debian
sudo apt install cmake pkg-config libopenh264-dev libssl-dev libyuv-dev

# Or build libyuv from source if not packaged
```

### 2. Build

```bash
# Build WebRTC server
cd web-streaming
make

# Build Basilisk II with IPC video driver
cd ../BasiliskII/src/Unix
./configure --enable-ipc-video
make
```

### 3. Configure

Set the screen mode to use the IPC driver in your prefs file:

```
screen ipc/640/480
```

### 4. Run

```bash
cd web-streaming
./build/macemu-webrtc
# Server starts emulator automatically and opens http://localhost:8000
```

Or run separately:

```bash
# Terminal 1: Start emulator
./BasiliskII --config myprefs

# Terminal 2: Start server (connects to running emulator)
./build/macemu-webrtc --no-auto-start
```

## Ports Used

| Port | Protocol | Purpose |
|------|----------|---------|
| 8000 | HTTP | Web client files |
| 8090 | WebSocket | WebRTC signaling |
| Dynamic | UDP | WebRTC media (ICE negotiated) |

## Command Line Options

```
Usage: macemu-webrtc [options]

Options:
  -h, --help              Show help
  -p, --http-port PORT    HTTP server port (default: 8000)
  -s, --signaling PORT    WebSocket signaling port (default: 8090)
  -e, --emulator PATH     Path to BasiliskII/SheepShaver executable
  -P, --prefs FILE        Emulator prefs file (default: basilisk_ii.prefs)
  -n, --no-auto-start     Don't auto-start emulator (wait for external)
  -t, --test-pattern      Generate test pattern (no emulator needed)
  --test-size WxH         Test pattern size (default: 640x480)
  --pid PID               Connect to specific emulator PID
  --roms PATH             ROMs directory (default: storage/roms)
  --images PATH           Disk images directory (default: storage/images)
```

## Input Handling

Mouse and keyboard events are sent via WebRTC DataChannel using a simple text protocol:

| Message | Format | Example |
|---------|--------|---------|
| Mouse move | `M{dx},{dy}` | `M5,-3` |
| Mouse down | `D{button}` | `D0` (left click) |
| Mouse up | `U{button}` | `U0` |
| Key down | `K{keycode}` | `K65` (A key) |
| Key up | `k{keycode}` | `k65` |

The server converts browser keycodes to Mac ADB scancodes.

### Supported Keys

- Letters A-Z
- Numbers 0-9
- Arrow keys
- Enter, Tab, Escape, Backspace, Delete, Space
- Modifier keys (Shift, Ctrl->Command, Alt->Option, Meta->Command)
- Common punctuation

## Troubleshooting

### Black screen / No video

1. Check browser console (F12) for WebRTC errors
2. Verify emulator is connected: look for `Video: Found and connected to emulator PID`
3. Check H264 stats in server console - should show IDR frames being encoded
4. Try the test pattern mode: `./build/macemu-webrtc --test-pattern`

### Yellow/wrong colors

- Ensure the 32-bit conversion uses `BGRAToI420` (Mac big-endian ARGB format)
- Check palette logging for indexed color modes

### Large frame sizes (>100KB IDR)

- Normal for 1-bit dithered startup screens
- Blur filter should reduce to ~40% of original
- Switch to 32-bit color mode for best compression

### Connection fails

- Ensure ports 8000 and 8090 are not blocked
- Check for WebSocket errors in browser console
- For remote access, may need TURN server configuration

### Input lag

- DataChannel uses unreliable mode for low latency
- Mouse moves are sent as deltas, not absolute positions
- Check network latency with browser DevTools

## Directory Structure

```
web-streaming/
├── libdatachannel/     # WebRTC library (git submodule)
├── server/
│   ├── server.cpp      # Main server implementation
│   └── ipc_protocol.h  # IPC protocol definitions
├── client/
│   ├── index.html      # Web client
│   ├── client.js       # WebRTC client code
│   └── styles.css      # Styling
├── build/              # Build output
└── Makefile

BasiliskII/src/IPC/
└── video_ipc.cpp       # IPC video driver for emulator
```

## Performance

Typical performance at 640x480:

| Metric | Value |
|--------|-------|
| Frame rate | 27-30 fps |
| IDR frame size | 30-40 KB |
| P frame size | 1-5 KB |
| Encoding time | <10ms |
| End-to-end latency | ~50-100ms |

## Dependencies

- **libdatachannel** - Built locally as git submodule
- **OpenH264** - H.264 encoding (`libopenh264-dev`)
- **libyuv** - Color conversion (`libyuv-dev`)
- **OpenSSL** - DTLS/SRTP (`libssl-dev`)
- **CMake** - Build system for libdatachannel

## See Also

- [IPC_DESIGN.md](IPC_DESIGN.md) - Detailed IPC protocol specification
- [libdatachannel](https://github.com/paullouisageneau/libdatachannel)
- [OpenH264](https://github.com/cisco/openh264)
- [libyuv](https://chromium.googlesource.com/libyuv/libyuv/)
