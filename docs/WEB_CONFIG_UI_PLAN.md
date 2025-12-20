# Web Configuration UI

Browser-based configuration interface for the WebRTC streaming client.

## Implementation Status

### Completed
- ✅ Settings modal with ROM/disk/RAM/screen selection
- ✅ ROM dropdown with known ROM database and checksums
- ✅ Disk image checkboxes for multi-select
- ✅ Auto-set Mac model based on ROM selection
- ✅ GET /api/config - read current configuration
- ✅ POST /api/config - save configuration
- ✅ GET /api/storage - list available ROMs and disks
- ✅ Full prefs file generation with all options
- ✅ Emulator start/stop/restart controls
- ✅ Connection status display
- ✅ WebRTC state debugging panel

### Pending
- ⬜ ROM file upload
- ⬜ Disk image upload
- ⬜ Network configuration (SLiRP, etc.)
- ⬜ Shared folder (ExtFS) configuration

## API Endpoints

### GET /api/config

Returns current configuration:
```json
{
  "rom": "1991-10 - 420DBFF3 - Quadra 700.ROM",
  "disks": ["7.6.img", "Apps.img"],
  "ram": 32,
  "screen": "800x600",
  "cpu": 4,
  "model": 14,
  "fpu": true,
  "jit": true,
  "sound": true
}
```

### POST /api/config

Save configuration (same format as GET response).

Returns: `{"success": true}` or `{"success": false, "error": "message"}`

### GET /api/storage

Returns available files:
```json
{
  "roms": [
    {"name": "Quadra700.ROM", "size": 1048576, "checksum": "420dbff3"}
  ],
  "disks": [
    {"name": "System7.img", "size": 104857600}
  ]
}
```

### GET /api/status

Returns emulator status:
```json
{
  "emulator_connected": true,
  "emulator_running": true,
  "emulator_pid": 12345,
  "video": {"width": 800, "height": 600, "frame_count": 1000}
}
```

### POST /api/emulator/start
### POST /api/emulator/stop
### POST /api/emulator/restart

Emulator lifecycle control.

## Configuration Options

### Basic Settings

| Setting | API Key | UI Element | Values |
|---------|---------|------------|--------|
| ROM | `rom` | Dropdown | From /api/storage |
| Disk Images | `disks` | Checkboxes | From /api/storage |
| RAM Size | `ram` | Dropdown | 8, 16, 32, 64, 128 MB |
| Resolution | `screen` | Dropdown | 640x480, 800x600, 1024x768 |

### Advanced Settings

| Setting | API Key | UI Element | Values |
|---------|---------|------------|--------|
| CPU Type | `cpu` | Dropdown | 2=68020, 3=68030, 4=68040 |
| Mac Model | `model` | Dropdown | Auto-set from ROM |
| FPU | `fpu` | Checkbox | Enable 68881/68882 FPU |
| JIT | `jit` | Checkbox | Enable JIT compiler |
| Sound | `sound` | Checkbox | Enable audio |

## Known ROM Database

The client includes a database of known Mac ROMs with checksums:

| Checksum | Model | Recommended |
|----------|-------|-------------|
| 97851db6 | Mac II | No |
| 368cadfe | Mac IIci | Yes |
| 420dbff3 | Quadra 700 | Yes |
| 3dc27823 | Quadra 900 | Yes |
| e33b2724 | Quadra 950 | Yes |
| ecfa989b | Quadra 800 | Yes |
| a49f9914 | Quadra 660AV | No |
| 9feb69b3 | Quadra 840AV | No |

When a known ROM is selected, the Mac model is automatically set to match.

## UI Layout

```
┌─────────────────────────────────────────────────────────────┐
│  🖥 Basilisk II Web  [libdatachannel]   ● Connected   ⚙️    │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│                      <video stream>                         │
│                                                             │
├─────────────────────────────────────────────────────────────┤
│  Emulator: ▶ Start  ⏹ Stop  🔄 Restart                      │
├─────────────────────────────────────────────────────────────┤
│  WebRTC State  │  Log Messages                              │
│  WS: open      │  [timestamp] Connected to server           │
│  PC: connected │  [timestamp] Video stream started          │
│  ICE: complete │                                            │
└─────────────────────────────────────────────────────────────┘

Settings Modal:
┌─────────────────────────────────────────────────────────────┐
│  Settings                                              [X]  │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  ROM File                                                   │
│  ┌─────────────────────────────────────────────────────┐   │
│  │ Quadra 700 (420DBFF3) - Recommended              ▼ │   │
│  └─────────────────────────────────────────────────────┘   │
│                                                             │
│  Disk Images                                                │
│  ┌─────────────────────────────────────────────────────┐   │
│  │ ☑ 7.6.img (500 MB)                                  │   │
│  │ ☐ Apps.img (100 MB)                                 │   │
│  │ ☐ Games.img (200 MB)                                │   │
│  └─────────────────────────────────────────────────────┘   │
│                                                             │
│  RAM: [32 MB ▼]    Screen: [800x600 ▼]                     │
│                                                             │
│  ▶ Advanced Settings                                        │
│  ┌─────────────────────────────────────────────────────┐   │
│  │ CPU: [68040 ▼]  Model: [Quadra 900 ▼]              │   │
│  │ ☑ FPU   ☑ JIT   ☑ Sound                            │   │
│  └─────────────────────────────────────────────────────┘   │
│                                                             │
│                              [Cancel]  [Save & Restart]     │
└─────────────────────────────────────────────────────────────┘
```

## Prefs File Format

Generated prefs file includes all standard Basilisk II options:

```
# Basilisk II preferences - generated by web UI

rom /path/to/storage/roms/Quadra700.ROM
disk /path/to/storage/images/7.6.img

# Hardware settings
ramsize 33554432
screen ipc/800/600
cpu 4
modelid 14
fpu true
jit true
nosound false

# JIT settings
jitfpu true
jitcachesize 8192
jitlazyflush true
jitinline true
jitdebug false

# Display settings
displaycolordepth 0
frameskip 0
scale_nearest false
scale_integer false

# Input settings
keyboardtype 5
keycodes false
mousewheelmode 1
mousewheellines 3
swap_opt_cmd true
hotkey 0

# Serial/Network
seriala /dev/null
serialb /dev/null
udptunnel false
udpport 6066

# Boot settings
bootdrive 0
bootdriver 0
nocdrom false

# System settings
ignoresegv true
idlewait true
noclipconversion false
nogui true

# SDL settings
sdlrender software
sdl_vsync true

# ExtFS settings
enableextfs false
extfs
```

## Reverse Proxy Support

The client supports running behind a reverse proxy:

1. **Relative API paths**: All API calls use relative URLs
2. **WebSocket URL override**: Use `?ws=wss://host/path` or `<meta name="ws-url">`
3. **HTTPS support**: Automatically uses `wss://` for secure connections

Example for VS Code port forwarding:
```
https://dev.example.com/proxy/8000/?ws=wss://dev.example.com/proxy/8090/
```
