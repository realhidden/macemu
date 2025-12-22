/*
 * H.264 Encoder using OpenH264
 */

#ifndef H264_ENCODER_H
#define H264_ENCODER_H

#include "codec.h"
#include <wels/codec_api.h>

class H264Encoder : public VideoCodec {
public:
    H264Encoder() = default;
    ~H264Encoder() override { cleanup(); }

    CodecType type() const override { return CodecType::H264; }
    const char* name() const override { return "H.264"; }

    bool init(int width, int height, int fps = 30) override;
    void cleanup() override;

    EncodedFrame encode_i420(const uint8_t* y, const uint8_t* u, const uint8_t* v,
                             int width, int height, int y_stride, int uv_stride) override;

    void request_keyframe() override { force_keyframe_ = true; }

    // Helper to check if raw H.264 data is a keyframe
    static bool is_keyframe(const std::vector<uint8_t>& data);

private:
    bool init_internal(int width, int height, int fps, int bitrate_kbps);

    ISVCEncoder* encoder_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int fps_ = 30;
    bool force_keyframe_ = true;  // Start true to force first frame as IDR
};

#endif // H264_ENCODER_H
