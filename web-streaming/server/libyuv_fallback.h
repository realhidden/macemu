/*
 * libyuv fallback implementations
 * Simple (non-SIMD) color conversion functions for when libyuv is unavailable.
 * These are slower than libyuv but work on any platform.
 */

#ifndef LIBYUV_FALLBACK_H
#define LIBYUV_FALLBACK_H

#include <stdint.h>

namespace libyuv {

// RGB to YUV conversion coefficients (BT.601)
// Y  =  0.299 R + 0.587 G + 0.114 B
// U  = -0.169 R - 0.331 G + 0.500 B + 128
// V  =  0.500 R - 0.419 G - 0.081 B + 128

static inline uint8_t clamp(int v) {
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

// Convert BGRA (bytes: B,G,R,A) to I420
// This is libyuv's "ARGB" format
static inline int ARGBToI420(
    const uint8_t* src_argb, int src_stride_argb,
    uint8_t* dst_y, int dst_stride_y,
    uint8_t* dst_u, int dst_stride_u,
    uint8_t* dst_v, int dst_stride_v,
    int width, int height)
{
    for (int y = 0; y < height; y++) {
        const uint8_t* src_row = src_argb + y * src_stride_argb;
        uint8_t* y_row = dst_y + y * dst_stride_y;

        for (int x = 0; x < width; x++) {
            int b = src_row[x * 4 + 0];
            int g = src_row[x * 4 + 1];
            int r = src_row[x * 4 + 2];
            // a = src_row[x * 4 + 3]; // unused

            // Y = 0.299*R + 0.587*G + 0.114*B
            y_row[x] = clamp((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
        }

        // Subsample U and V (every 2x2 block)
        if (y % 2 == 0) {
            uint8_t* u_row = dst_u + (y / 2) * dst_stride_u;
            uint8_t* v_row = dst_v + (y / 2) * dst_stride_v;

            for (int x = 0; x < width; x += 2) {
                // Average 2x2 block (or 2x1 if at bottom edge)
                int b = src_row[x * 4 + 0];
                int g = src_row[x * 4 + 1];
                int r = src_row[x * 4 + 2];

                if (x + 1 < width) {
                    b = (b + src_row[(x + 1) * 4 + 0]) / 2;
                    g = (g + src_row[(x + 1) * 4 + 1]) / 2;
                    r = (r + src_row[(x + 1) * 4 + 2]) / 2;
                }

                // U = -0.169*R - 0.331*G + 0.500*B + 128
                // V =  0.500*R - 0.419*G - 0.081*B + 128
                u_row[x / 2] = clamp(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
                v_row[x / 2] = clamp(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
            }
        }
    }
    return 0;
}

// Convert ARGB (bytes: A,R,G,B) to I420
// This is libyuv's "BGRA" format (Mac native 32-bit)
static inline int BGRAToI420(
    const uint8_t* src_bgra, int src_stride_bgra,
    uint8_t* dst_y, int dst_stride_y,
    uint8_t* dst_u, int dst_stride_u,
    uint8_t* dst_v, int dst_stride_v,
    int width, int height)
{
    for (int y = 0; y < height; y++) {
        const uint8_t* src_row = src_bgra + y * src_stride_bgra;
        uint8_t* y_row = dst_y + y * dst_stride_y;

        for (int x = 0; x < width; x++) {
            // ARGB bytes: A, R, G, B at offsets 0, 1, 2, 3
            int r = src_row[x * 4 + 1];
            int g = src_row[x * 4 + 2];
            int b = src_row[x * 4 + 3];

            y_row[x] = clamp((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
        }

        if (y % 2 == 0) {
            uint8_t* u_row = dst_u + (y / 2) * dst_stride_u;
            uint8_t* v_row = dst_v + (y / 2) * dst_stride_v;

            for (int x = 0; x < width; x += 2) {
                int r = src_row[x * 4 + 1];
                int g = src_row[x * 4 + 2];
                int b = src_row[x * 4 + 3];

                if (x + 1 < width) {
                    r = (r + src_row[(x + 1) * 4 + 1]) / 2;
                    g = (g + src_row[(x + 1) * 4 + 2]) / 2;
                    b = (b + src_row[(x + 1) * 4 + 3]) / 2;
                }

                u_row[x / 2] = clamp(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
                v_row[x / 2] = clamp(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
            }
        }
    }
    return 0;
}

// Convert I420 to RGB (RAW format: R,G,B bytes)
static inline int I420ToRAW(
    const uint8_t* src_y, int src_stride_y,
    const uint8_t* src_u, int src_stride_u,
    const uint8_t* src_v, int src_stride_v,
    uint8_t* dst_raw, int dst_stride_raw,
    int width, int height)
{
    for (int y = 0; y < height; y++) {
        const uint8_t* y_row = src_y + y * src_stride_y;
        const uint8_t* u_row = src_u + (y / 2) * src_stride_u;
        const uint8_t* v_row = src_v + (y / 2) * src_stride_v;
        uint8_t* dst_row = dst_raw + y * dst_stride_raw;

        for (int x = 0; x < width; x++) {
            int Y = y_row[x] - 16;
            int U = u_row[x / 2] - 128;
            int V = v_row[x / 2] - 128;

            // R = 1.164*Y + 1.596*V
            // G = 1.164*Y - 0.392*U - 0.813*V
            // B = 1.164*Y + 2.017*U
            int r = (298 * Y + 409 * V + 128) >> 8;
            int g = (298 * Y - 100 * U - 208 * V + 128) >> 8;
            int b = (298 * Y + 516 * U + 128) >> 8;

            dst_row[x * 3 + 0] = clamp(r);
            dst_row[x * 3 + 1] = clamp(g);
            dst_row[x * 3 + 2] = clamp(b);
        }
    }
    return 0;
}

// Convert BGRA (bytes: B,G,R,A) to RGB (RAW format: R,G,B bytes)
// This is libyuv's "ARGB" format
static inline int ARGBToRAW(
    const uint8_t* src_argb, int src_stride_argb,
    uint8_t* dst_raw, int dst_stride_raw,
    int width, int height)
{
    for (int y = 0; y < height; y++) {
        const uint8_t* src_row = src_argb + y * src_stride_argb;
        uint8_t* dst_row = dst_raw + y * dst_stride_raw;

        for (int x = 0; x < width; x++) {
            // BGRA bytes: B, G, R, A -> RGB bytes: R, G, B
            dst_row[x * 3 + 0] = src_row[x * 4 + 2];  // R
            dst_row[x * 3 + 1] = src_row[x * 4 + 1];  // G
            dst_row[x * 3 + 2] = src_row[x * 4 + 0];  // B
        }
    }
    return 0;
}

} // namespace libyuv

#endif // LIBYUV_FALLBACK_H
