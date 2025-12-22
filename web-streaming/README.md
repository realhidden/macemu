# Basilisk II Web Streaming

Browser-based access to Basilisk II / SheepShaver running in headless mode using WebRTC.

## Architecture

```
┌─────────────────────┐         ┌─────────────────────┐         ┌──────────────┐
│     Emulator        │   IPC   │   WebRTC Server     │  HTTP/  │   Browser    │
│ (BasiliskII/        │◄───────►│                     │  WS     │              │
│  SheepShaver)       │         │ macemu-webrtc       │◄───────►│ Web Client   │
└─────────────────────┘         └─────────────────────┘         └──────────────┘
        │                               │                              │
        │ Creates:                      │ Connects to:                 │
        │ - SHM /macemu-video-{PID}     │ - Emulator SHM (read)        │
        │ - Socket /tmp/macemu-{PID}.sock│ - Emulator socket (write)   │
        │                               │                              │
        │ Outputs:                      │ Provides:                    │
        │ - BGRA frames (triple-buffered)│ - H.264 via WebRTC Track   │
        │ - Accepts binary input        │ - PNG via DataChannel        │
        │                               │ - Input relay to emulator    │
        └───────────────────────────────┴──────────────────────────────┘
```

### Key Components

- **Emulator OWNS resources**: Creates SHM and Unix socket at startup
- **Server CONNECTS**: Discovers emulators by scanning `/dev/shm/macemu-video-*`
- **Triple-buffered video**: Lock-free, no polling - atomics for synchronization
- **Dual codec support**: H.264 (RTP track) or PNG (DataChannel) - server-configured
- **Binary input protocol**: Efficient mouse/keyboard relay with latency tracking

## Quick Start

```bash
# Install system dependencies
sudo apt install cmake pkg-config libopenh264-dev libyuv-dev libssl-dev

# Build libdatachannel and WebRTC server
cd web-streaming
make

# Build Basilisk II with IPC video support
cd ../BasiliskII/src/Unix
./configure --enable-ipc-video
make

# Run the server (auto-starts emulator)
cd ../../../web-streaming
./build/macemu-webrtc

# Open http://localhost:8000 in your browser
```

## IPC Protocol (Version 4)

### Shared Memory Layout

```c
typedef struct {
    // Header - validated on connect
    uint32_t magic;              // 0x4D454D34 ("MEM4")
    uint32_t version;            // 4
    uint32_t pid;                // Emulator PID
    uint32_t state;              // Running/paused/stopped

    // Frame dimensions
    uint32_t width, height;      // Actual size (≤1920x1080)
    uint32_t pixel_format;       // Always BGRA (B,G,R,A bytes)

    // Triple buffer sync (lock-free atomics)
    atomic_uint32 write_index;   // Emulator writes here
    atomic_uint32 ready_index;   // Server reads here
    atomic_uint64 frame_count;   // Monotonic counter
    atomic_uint64 timestamp_us;  // Frame completion time

    // Latency stats
    atomic_uint32 mouse_latency_avg_ms;  // x10 for 0.1ms precision
    atomic_uint32 mouse_latency_samples;

    // Frame buffers (3 × 1920×1080×4 = ~24.9 MB)
    uint8_t frames[3][MACEMU_BGRA_FRAME_SIZE];
} MacEmuVideoBuffer;
```

### Binary Input Protocol

Sent over Unix socket from server to emulator:

| Message Type | Size | Fields |
|--------------|------|--------|
| Key | 8 bytes | type, flags, mac_keycode, modifiers |
| Mouse | 20 bytes | type, flags, x, y, buttons, timestamp_ms |
| Command | 8 bytes | type, flags, command (start/stop/reset/pause) |

### Resource Naming

- **SHM**: `/macemu-video-{PID}` (POSIX shared memory)
- **Socket**: `/tmp/macemu-{PID}.sock` (Unix domain socket)

## Video Pipeline

### Emulator Side (video_ipc.cpp)

1. Mac OS renders to framebuffer (1/2/4/8/16/32-bit color)
2. Convert any depth to BGRA (B,G,R,A bytes = libyuv "ARGB")
3. Write to `frames[write_index]` in SHM
4. Call `macemu_frame_complete()` - atomic buffer swap

### Server Side (server.cpp)

1. Map emulator's SHM (read-only)
2. Poll `frame_count` for new frames
3. Read from `frames[ready_index]`
4. Encode:
   - **H.264**: BGRA → I420 (libyuv) → H.264 (OpenH264) → RTP
   - **PNG**: BGRA → RGB (libyuv) → PNG (fpng) → DataChannel

### Browser Side (client.js)

- **H.264**: WebRTC video track → `<video>` element (hardware decode)
- **PNG**: DataChannel binary → `createImageBitmap()` → canvas

## Latency Measurement

### Mouse Input Latency (Browser → Emulator)

1. Browser sends `performance.now()` timestamp with mouse events
2. Server forwards timestamp in binary mouse message
3. Emulator syncs clocks on first message (epoch offset)
4. Emulator calculates latency and writes to SHM
5. Server exposes via `/api/status`, displayed in browser UI

### Video Latency (Server → Browser)

1. Server prepends 8-byte timestamp to PNG frames
2. Browser syncs clocks on first frame (epoch offset)
3. Browser calculates receive_time - expected_time
4. Displayed in stats panel

## Codec Selection

Set in `basilisk_ii.prefs`:

```
# Video codec for web streaming (png or h264)
webcodec h264
```

| Codec | Transport | Best For |
|-------|-----------|----------|
| H.264 | WebRTC RTP Track | Low bandwidth, hardware decode |
| PNG | WebRTC DataChannel | Pixel-perfect, high-color content |

## Directory Structure

```
web-streaming/
├── server/
│   ├── server.cpp          # WebRTC server, HTTP, IPC client
│   ├── ipc_protocol.h      # Shared memory & binary protocol
│   ├── codec.h             # Video codec abstraction
│   ├── h264_encoder.cpp/h  # H.264 encoding (OpenH264)
│   ├── png_encoder.cpp/h   # PNG encoding (fpng)
│   └── fpng.cpp/h          # Fast PNG library
├── client/
│   ├── index.html          # Web UI
│   ├── client.js           # WebRTC client, video decoders
│   └── styles.css          # UI styles
├── build/                  # Build output
├── libdatachannel/         # WebRTC library (git submodule)
├── storage/
│   ├── roms/               # Mac ROM files
│   └── images/             # Disk images
├── basilisk_ii.prefs       # Emulator config
├── Makefile
└── README.md
```

## Build Targets

| Target | Description |
|--------|-------------|
| `all` | Build everything (default) |
| `libdatachannel` | Build WebRTC library only |
| `clean` | Remove build files |
| `distclean` | Remove all build files including libdatachannel |
| `deps` | Install system dependencies (Ubuntu/Debian) |
| `deps-check` | Verify dependencies are installed |
| `run` | Build and run the server |

## API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Web client |
| `/api/status` | GET | Emulator status, latency stats |
| `/api/storage` | GET | Available ROMs and disk images |
| `/api/prefs` | GET/POST | Read/write emulator preferences |
| `/api/emulator/start` | POST | Start emulator |
| `/api/emulator/stop` | POST | Stop emulator |
| `/api/emulator/restart` | POST | Restart emulator |
| `/api/log` | POST | Browser log relay |

## Command Line Options

```
Usage: macemu-webrtc [options]

Options:
  -h, --help              Show help
  -p, --http-port PORT    HTTP server port (default: 8000)
  -s, --signaling PORT    WebSocket signaling port (default: 8090)
  -e, --emulator PATH     Path to BasiliskII/SheepShaver executable
  -P, --prefs FILE        Emulator prefs file (default: basilisk_ii.prefs)
  -n, --no-auto-start     Don't auto-start emulator
  --pid PID               Connect to specific emulator PID
  --roms PATH             ROMs directory (default: storage/roms)
  --images PATH           Disk images directory (default: storage/images)
```

## Dependencies

### System Packages (apt)

- **libopenh264-dev** - H.264 encoder
- **libyuv-dev** - Color space conversion
- **libssl-dev** - TLS/crypto for DTLS
- **cmake** - Build system for libdatachannel

### Bundled

- **libdatachannel** - WebRTC library (git submodule)
- **fpng** - Fast PNG encoder (included source)

## Ports

| Port | Protocol | Purpose |
|------|----------|---------|
| 8000 | HTTP | Web client |
| 8090 | WebSocket | WebRTC signaling |
| Dynamic | UDP | WebRTC media (ICE negotiated) |

## Troubleshooting

### No video displayed

1. Check emulator is running: `ls /dev/shm/macemu-video-*`
2. Check socket exists: `ls /tmp/macemu-*.sock`
3. Check server logs for "Connected to video SHM"
4. Ensure `screen ipc/WIDTH/HEIGHT` in prefs file

### High latency

1. Check stats panel for mouse/video latency
2. Use H.264 codec for lower latency
3. Reduce resolution if needed

### Build errors

```bash
# Check all dependencies
make deps-check

# Install missing dependencies
make deps
```
