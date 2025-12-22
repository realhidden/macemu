/*
 * Video Codec Abstraction
 *
 * Allows switching between different encoding strategies:
 * - H.264 via OpenH264 (WebRTC video track)
 * - PNG for dithered content (DataChannel binary)
 * - Raw RGBA (DataChannel binary, highest bandwidth)
 */

#ifndef CODEC_H
#define CODEC_H

#include <vector>
#include <cstdint>

enum class CodecType {
    H264,       // WebRTC video track with H.264
    PNG,        // PNG over DataChannel (good for dithered)
    RAW         // Raw RGBA over DataChannel (lowest latency)
};

struct EncodedFrame {
    std::vector<uint8_t> data;
    bool is_keyframe = false;
    CodecType codec = CodecType::H264;
    int width = 0;
    int height = 0;
};

class VideoCodec {
public:
    virtual ~VideoCodec() = default;

    // Get codec type
    virtual CodecType type() const = 0;

    // Get codec name for display/logging
    virtual const char* name() const = 0;

    // Initialize with resolution and parameters
    virtual bool init(int width, int height, int fps = 30) = 0;

    // Cleanup resources
    virtual void cleanup() = 0;

    // Encode a frame from I420 data (from SHM)
    virtual EncodedFrame encode_i420(const uint8_t* y, const uint8_t* u, const uint8_t* v,
                                     int width, int height, int y_stride, int uv_stride) = 0;

    // Encode from BGRA (for codecs that prefer RGB input)
    virtual EncodedFrame encode_bgra(const uint8_t* bgra, int width, int height, int stride) {
        (void)bgra; (void)width; (void)height; (void)stride;
        return EncodedFrame{};  // Default: not supported
    }

    // Request keyframe on next encode
    virtual void request_keyframe() = 0;
};

#endif // CODEC_H
