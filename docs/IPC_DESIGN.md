# IPC Design for Web Streaming

Architecture for the standalone WebRTC server that communicates with BasiliskII/SheepShaver via IPC.

## Overview

```
┌─────────────────┐         IPC          ┌─────────────────┐
│   BasiliskII    │◄───────────────────►│  standalone     │
│  or SheepShaver │                      │     server      │
└─────────────────┘                      └─────────────────┘
        │                                        │
        ▼                                        ▼
   Mac OS Guest                            Web Browsers
```

## Implementation Status

- ✅ Shared memory for video frames (triple-buffered)
- ✅ Shared memory for audio (ring buffer)
- ✅ Unix domain socket for input/control
- ✅ Standalone WebRTC server
- ✅ IPC video driver for emulator
- ✅ Web configuration UI

## IPC Mechanisms

### Video Frames: POSIX Shared Memory

**Location**: `/dev/shm/macemu-video-{pid}`

High-bandwidth video (~57 MB/s at 800×600×4×30fps) uses shared memory for zero-copy transfer.

```cpp
struct MacEmuVideoBuffer {
    uint32_t magic;           // MACEMU_VIDEO_MAGIC
    uint32_t version;         // MACEMU_IPC_VERSION (1)
    uint32_t width;           // Frame width
    uint32_t height;          // Frame height
    uint32_t stride;          // Bytes per row (width * 4)
    uint32_t format;          // 0 = RGBA
    std::atomic<uint32_t> write_index;   // Triple buffer index (0-2)
    std::atomic<uint64_t> frame_count;   // Total frames written
    uint8_t frames[3][MAX_FRAME_SIZE];   // Triple-buffered RGBA
};
```

**Triple Buffering Protocol**:
1. Emulator writes to `(write_index + 1) % 3`
2. After write, atomically updates `write_index`
3. Server reads from `write_index`
4. Lock-free, no tearing, no blocking

### Audio: Shared Memory Ring Buffer

**Location**: `/dev/shm/macemu-audio-{pid}`

```cpp
struct MacEmuAudioBuffer {
    uint32_t magic;           // MACEMU_AUDIO_MAGIC
    uint32_t version;         // MACEMU_IPC_VERSION
    uint32_t sample_rate;     // e.g., 44100
    uint32_t channels;        // 1 or 2
    uint32_t format;          // 0=S16LE
    uint32_t buffer_size;     // Ring buffer size
    std::atomic<uint32_t> write_pos;
    std::atomic<uint32_t> read_pos;
    uint8_t ring_buffer[65536];
};
```

### Input/Control: Unix Domain Socket

**Location**: `/tmp/macemu-{pid}.sock`

Bidirectional JSON messages over Unix socket.

#### Server → Emulator

```json
{"type": "mouse", "x": 100, "y": 200, "buttons": 1}
{"type": "key", "code": 65, "down": true}
{"type": "restart"}
```

#### Emulator → Server

```json
{"type": "resolution", "width": 800, "height": 600}
{"type": "ack"}
```

## File Structure

```
web-streaming/
├── server/
│   ├── standalone_server.cpp   # Main server (WebRTC, HTTP, IPC)
│   └── ipc_protocol.h          # Shared memory structures
├── client/
│   ├── index_datachannel.html  # Web UI
│   ├── datachannel_client.js   # WebRTC client
│   └── styles.css              # Styling
├── storage/
│   ├── roms/                   # ROM files
│   └── images/                 # Disk images
└── Makefile

BasiliskII/src/IPC/
└── video_ipc.cpp               # IPC video driver
```

## Building

### Server

```bash
cd web-streaming
make
```

Dependencies: libdatachannel, libvpx

### Emulator with IPC

```bash
cd BasiliskII/src/Unix
./configure --enable-ipc-video
make
```

## Running

### Option 1: Server manages emulator (recommended)

```bash
cd web-streaming
./server/standalone_server --emulator ../BasiliskII/src/Unix/BasiliskII
```

Server auto-starts emulator, manages lifecycle, restarts on crash.

### Option 2: Manual startup

Terminal 1:
```bash
./server/standalone_server --no-auto-start
```

Terminal 2:
```bash
MACEMU_VIDEO_SHM=/macemu-video-{pid} \
MACEMU_CONTROL_SOCK=/tmp/macemu-{pid}.sock \
./BasiliskII --config prefs
```

## Command Line Options

| Option | Default | Description |
|--------|---------|-------------|
| `-p, --http-port` | 8000 | HTTP server port |
| `-s, --signaling` | 8090 | WebSocket signaling port |
| `-e, --emulator` | auto | Path to emulator executable |
| `-P, --prefs` | basilisk_ii.prefs | Prefs file path |
| `-n, --no-auto-start` | false | Don't auto-start emulator |
| `--roms` | storage/roms | ROM directory |
| `--images` | storage/images | Disk images directory |

## Environment Variables

| Variable | Description |
|----------|-------------|
| `MACEMU_VIDEO_SHM` | Override video shared memory name |
| `MACEMU_CONTROL_SOCK` | Override control socket path |
| `BASILISK_ROMS` | ROM directory |
| `BASILISK_IMAGES` | Disk images directory |

## Video Pipeline

```
┌──────────┐    ┌──────────┐    ┌──────────┐    ┌──────────┐
│ Emulator │───►│ Shared   │───►│ VP8      │───►│ WebRTC   │
│ (RGBA)   │    │ Memory   │    │ Encoder  │    │ RTP/UDP  │
└──────────┘    └──────────┘    └──────────┘    └──────────┘
                     │
              Zero-copy transfer
```

1. Emulator writes RGBA frame to shared memory
2. Server reads frame, converts RGBA → I420
3. VP8 encoder compresses frame
4. RTP packetizer sends over WebRTC data channel

## Benefits

1. **Process isolation**: Server can restart without affecting emulator
2. **Shared codebase**: Same server works with BasiliskII and SheepShaver
3. **Resource management**: Encoding load is separate from emulation
4. **Zero-copy video**: Shared memory eliminates frame copies
5. **Flexible deployment**: Emulator and server can run on different hosts (with network transport)
