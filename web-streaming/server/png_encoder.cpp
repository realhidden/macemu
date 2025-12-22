/*
 * PNG Encoder using fpng (fast PNG)
 * fpng is ~10x faster than libpng for encoding
 */

#include "png_encoder.h"
#include "fpng.h"
#include <cstring>
#include <cstdio>

// libyuv for fast I420 to RGB conversion
#include <libyuv.h>

// Initialize fpng once
static bool g_fpng_initialized = false;

bool PNGEncoder::init(int width, int height, int fps) {
    cleanup();

    // Initialize fpng (only once)
    if (!g_fpng_initialized) {
        fpng::fpng_init();
        g_fpng_initialized = true;
        fprintf(stderr, "PNG: fpng initialized (SSE4.1: %s)\n",
                fpng::fpng_cpu_supports_sse41() ? "yes" : "no");
    }

    width_ = width;
    height_ = height;
    fps_ = fps;

    // Pre-allocate RGB buffer
    rgb_buffer_.resize(width * height * 3);  // RGB24

    fprintf(stderr, "PNG: Encoder initialized %dx%d (using fpng)\n", width, height);
    return true;
}

void PNGEncoder::cleanup() {
    rgb_buffer_.clear();
    png_buffer_.clear();
    width_ = 0;
    height_ = 0;
    frame_count_ = 0;
    total_size_ = 0;
}

void PNGEncoder::i420_to_rgb(const uint8_t* y, const uint8_t* u, const uint8_t* v,
                              int width, int height, int y_stride, int uv_stride) {
    // Use libyuv for fast conversion
    // fpng expects RGB (R first), so use I420ToRAW
    libyuv::I420ToRAW(
        y, y_stride,
        u, uv_stride,
        v, uv_stride,
        rgb_buffer_.data(), width * 3,
        width, height
    );
}

bool PNGEncoder::encode_rgb_to_png(int width, int height) {
    png_buffer_.clear();

    // fpng_encode_image_to_memory with FPNG_ENCODE_SLOWER flag
    // This gives ~6% smaller files at ~40% slower encoding speed
    // Still much faster than libpng, and smaller files = less bandwidth
    bool success = fpng::fpng_encode_image_to_memory(
        rgb_buffer_.data(),
        width,
        height,
        3,  // num_chans: RGB
        png_buffer_,
        fpng::FPNG_ENCODE_SLOWER  // Better compression
    );

    if (!success) {
        fprintf(stderr, "PNG: fpng encode failed\n");
        return false;
    }

    return true;
}

EncodedFrame PNGEncoder::encode_i420(const uint8_t* y, const uint8_t* u, const uint8_t* v,
                                      int width, int height, int y_stride, int uv_stride) {
    EncodedFrame result;
    result.codec = CodecType::PNG;
    result.width = width;
    result.height = height;
    result.is_keyframe = true;  // PNG frames are always keyframes

    // Reinit if size changed
    if (width != width_ || height != height_) {
        if (!init(width, height)) {
            return result;
        }
    }

    // Convert I420 to RGB
    i420_to_rgb(y, u, v, width, height, y_stride, uv_stride);

    // Encode to PNG using fpng
    if (!encode_rgb_to_png(width, height)) {
        return result;
    }

    result.data = std::move(png_buffer_);
    png_buffer_.clear();  // Reset after move

    // Stats
    frame_count_++;
    total_size_ += result.data.size();

    // Log every 30 frames
    if (frame_count_ % 30 == 0) {
        float avg_size = static_cast<float>(total_size_) / frame_count_;
        fprintf(stderr, "PNG: frame=%d size=%zu bytes (avg %.1f KB)\n",
                frame_count_, result.data.size(), avg_size / 1024.0f);
    }

    return result;
}

EncodedFrame PNGEncoder::encode_bgra(const uint8_t* bgra, int width, int height, int stride) {
    EncodedFrame result;
    result.codec = CodecType::PNG;
    result.width = width;
    result.height = height;
    result.is_keyframe = true;

    // Reinit if size changed
    if (width != width_ || height != height_) {
        if (!init(width, height)) {
            return result;
        }
    }

    // Convert BGRA to RGB using libyuv
    // BGRA = bytes B,G,R,A = libyuv "ARGB"
    // ARGBToRAW converts to RGB
    libyuv::ARGBToRAW(
        bgra, stride,
        rgb_buffer_.data(), width * 3,
        width, height
    );

    // Encode to PNG using fpng
    if (!encode_rgb_to_png(width, height)) {
        return result;
    }

    result.data = std::move(png_buffer_);
    png_buffer_.clear();

    frame_count_++;
    total_size_ += result.data.size();

    return result;
}

EncodedFrame PNGEncoder::encode_argb(const uint8_t* argb, int width, int height, int stride) {
    EncodedFrame result;
    result.codec = CodecType::PNG;
    result.width = width;
    result.height = height;
    result.is_keyframe = true;

    // Reinit if size changed
    if (width != width_ || height != height_) {
        if (!init(width, height)) {
            return result;
        }
    }

    // Convert ARGB to RGB manually
    // ARGB = bytes A,R,G,B in memory (Mac native 32-bit)
    // RGB = bytes R,G,B in memory
    // libyuv doesn't have a direct function for this, so we do it manually
    uint8_t* dst = rgb_buffer_.data();
    for (int row = 0; row < height; row++) {
        const uint8_t* src_row = argb + row * stride;
        uint8_t* dst_row = dst + row * width * 3;
        for (int col = 0; col < width; col++) {
            // ARGB bytes: A, R, G, B at offsets 0, 1, 2, 3
            dst_row[col * 3 + 0] = src_row[col * 4 + 1];  // R
            dst_row[col * 3 + 1] = src_row[col * 4 + 2];  // G
            dst_row[col * 3 + 2] = src_row[col * 4 + 3];  // B
        }
    }

    // Encode to PNG using fpng
    if (!encode_rgb_to_png(width, height)) {
        return result;
    }

    result.data = std::move(png_buffer_);
    png_buffer_.clear();

    frame_count_++;
    total_size_ += result.data.size();

    return result;
}
