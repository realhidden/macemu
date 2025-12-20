/*
 * IPC Protocol for macemu WebRTC Streaming
 *
 * Defines shared memory structures for video/audio transfer
 * and control socket message formats.
 */

#ifndef IPC_PROTOCOL_H
#define IPC_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
#include <atomic>
#define ATOMIC_UINT32 std::atomic<uint32_t>
#define ATOMIC_UINT64 std::atomic<uint64_t>
#define ATOMIC_LOAD(ptr) (ptr).load()
#define ATOMIC_STORE(ptr, val) (ptr).store(val)
#define ATOMIC_FETCH_ADD(ptr, val) (ptr).fetch_add(val)
#else
#include <stdatomic.h>
#define ATOMIC_UINT32 _Atomic uint32_t
#define ATOMIC_UINT64 _Atomic uint64_t
#define ATOMIC_LOAD(ptr) atomic_load(&(ptr))
#define ATOMIC_STORE(ptr, val) atomic_store(&(ptr), val)
#define ATOMIC_FETCH_ADD(ptr, val) atomic_fetch_add(&(ptr), val)
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Shared memory names (can be overridden via environment)
#define MACEMU_VIDEO_SHM_DEFAULT "/macemu-video"
#define MACEMU_AUDIO_SHM_DEFAULT "/macemu-audio"
#define MACEMU_CONTROL_SOCK_DEFAULT "/tmp/macemu-control.sock"

// Magic numbers for validation
#define MACEMU_VIDEO_MAGIC 0x4D454D55  // "MEMU"
#define MACEMU_AUDIO_MAGIC 0x4D415544  // "MAUD"

// Protocol version
#define MACEMU_IPC_VERSION 1

// Maximum frame size (4K @ 32bpp)
#define MACEMU_MAX_FRAME_SIZE (3840 * 2160 * 4)

// Audio buffer size (64KB = ~370ms at 44.1kHz stereo 16-bit)
#define MACEMU_AUDIO_BUFFER_SIZE 65536

// Audio formats
#define MACEMU_AUDIO_S16LE 0
#define MACEMU_AUDIO_F32LE 1

/*
 * Shared Video Buffer
 *
 * Triple-buffered video frames in shared memory.
 * Emulator writes frames, server reads for encoding.
 *
 * Protocol:
 * 1. Emulator calculates next = (write_index + 1) % 3
 * 2. Emulator writes frame to frames[next]
 * 3. Emulator atomically updates write_index to next
 * 4. Server reads from frames[write_index] (current)
 *
 * No locks needed - writer never touches current read buffer.
 */
typedef struct {
    uint32_t magic;              // Must be MACEMU_VIDEO_MAGIC
    uint32_t version;            // Protocol version
    uint32_t width;              // Frame width in pixels
    uint32_t height;             // Frame height in pixels
    uint32_t stride;             // Bytes per row (usually width * 4)
    uint32_t format;             // 0 = RGBA, 1 = BGRA
    uint32_t _reserved[2];       // Future use

    ATOMIC_UINT32 write_index;   // Current write buffer index (0-2)
    ATOMIC_UINT32 read_index;    // Last buffer read by server
    ATOMIC_UINT64 frame_count;   // Total frames written (monotonic)
    ATOMIC_UINT64 timestamp_us;  // Timestamp of current frame (microseconds)

    // Triple buffer for frames
    // Actual size depends on resolution, but we allocate max
    uint8_t frames[3][MACEMU_MAX_FRAME_SIZE];
} MacEmuVideoBuffer;

/*
 * Shared Audio Buffer
 *
 * Ring buffer for audio samples.
 * Emulator writes samples, server reads for encoding.
 *
 * Protocol:
 * - write_pos: next byte to write (emulator advances)
 * - read_pos: next byte to read (server advances)
 * - Available data: (write_pos - read_pos) mod buffer_size
 * - Free space: buffer_size - available - 1
 */
typedef struct {
    uint32_t magic;              // Must be MACEMU_AUDIO_MAGIC
    uint32_t version;            // Protocol version
    uint32_t sample_rate;        // e.g., 44100
    uint32_t channels;           // 1 = mono, 2 = stereo
    uint32_t format;             // MACEMU_AUDIO_S16LE or MACEMU_AUDIO_F32LE
    uint32_t buffer_size;        // Ring buffer size (always MACEMU_AUDIO_BUFFER_SIZE)
    uint32_t _reserved[2];       // Future use

    ATOMIC_UINT32 write_pos;     // Write position in ring buffer
    ATOMIC_UINT32 read_pos;      // Read position in ring buffer
    ATOMIC_UINT64 sample_count;  // Total samples written (monotonic)

    uint8_t ring_buffer[MACEMU_AUDIO_BUFFER_SIZE];
} MacEmuAudioBuffer;

/*
 * Control Socket Messages
 *
 * JSON messages over Unix domain socket, newline-delimited.
 * Server → Emulator: input events, config requests
 * Emulator → Server: config data, status, errors
 */

// Message types (for documentation - actual messages are JSON strings)
// Server → Emulator:
//   {"type":"mouse_move","x":100,"y":200}
//   {"type":"mouse_button","x":100,"y":200,"button":0,"pressed":true}
//   {"type":"key","code":65,"pressed":true,"ctrl":false,"alt":false,"shift":false,"meta":false}
//   {"type":"get_config"}
//   {"type":"set_config","config":{...}}
//   {"type":"restart"}
//   {"type":"shutdown"}
//
// Emulator → Server:
//   {"type":"config","data":{...}}
//   {"type":"storage","roms":[...],"disks":[...]}
//   {"type":"status","running":true,"fps":30}
//   {"type":"error","message":"..."}
//   {"type":"ack"}

/*
 * Helper functions for shared memory
 */

// Calculate actual frame size for given dimensions
static inline size_t macemu_frame_size(uint32_t width, uint32_t height) {
    return (size_t)width * height * 4;
}

// Calculate total shared video buffer size
static inline size_t macemu_video_buffer_size(void) {
    return sizeof(MacEmuVideoBuffer);
}

// Calculate total shared audio buffer size
static inline size_t macemu_audio_buffer_size(void) {
    return sizeof(MacEmuAudioBuffer);
}

// Get available audio data in ring buffer
static inline uint32_t macemu_audio_available(const MacEmuAudioBuffer* buf) {
    uint32_t write_val = ATOMIC_LOAD(buf->write_pos);
    uint32_t read_val = ATOMIC_LOAD(buf->read_pos);
    return (write_val - read_val) % buf->buffer_size;
}

// Get free space in audio ring buffer
static inline uint32_t macemu_audio_free(const MacEmuAudioBuffer* buf) {
    return buf->buffer_size - macemu_audio_available(buf) - 1;
}

#ifdef __cplusplus
}
#endif

#endif // IPC_PROTOCOL_H
