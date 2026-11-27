// CPU reference for shaders/core/convert.slang. The shader tests compare the
// GPU kernels against these within a per-format tolerance (architecture.md,
// "Testing": shader layer).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace engine::convert_ref {

struct Pixel { float r, g, b, a; };  // linear, premultiplied

inline float encode_srgb(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

// Rec. 709 limited range on sRGB-encoded values, normalized so 8-bit code = v * 255.
inline void ycbcr(const Pixel& p, float& y, float& cb, float& cr) {
    const float r = encode_srgb(p.r), g = encode_srgb(p.g), b = encode_srgb(p.b);
    const float yy = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    y = 16.0f / 255 + yy * 219.0f / 255;
    cb = 128.0f / 255 + (b - yy) / 1.8556f * 224.0f / 255;
    cr = 128.0f / 255 + (r - yy) / 1.5748f * 224.0f / 255;
}

inline uint32_t code8(float v) { return static_cast<uint32_t>(std::lround(v * 255.0f)); }
inline uint32_t code10(float v) {
    return std::clamp<uint32_t>(static_cast<uint32_t>(std::lround(v * 1020.0f)), 4u, 1019u);
}

// UYVY: one word per pixel pair, bytes U Y0 V Y1.
inline std::vector<uint32_t> to_uyvy(const std::vector<Pixel>& px, uint32_t w, uint32_t h, uint32_t row_words) {
    std::vector<uint32_t> out(static_cast<size_t>(row_words) * h, 0);
    for (uint32_t yy = 0; yy < h; ++yy)
        for (uint32_t x = 0; x < w; x += 2) {
            float y0, cb0, cr0, y1, cb1, cr1;
            ycbcr(px[yy * w + x], y0, cb0, cr0);
            ycbcr(px[yy * w + std::min(x + 1, w - 1)], y1, cb1, cr1);
            out[yy * row_words + x / 2] = code8((cb0 + cb1) / 2) | (code8(y0) << 8) |
                                          (code8((cr0 + cr1) / 2) << 16) | (code8(y1) << 24);
        }
    return out;
}

// v210: 6 pixels in 4 little-endian words of three 10-bit components.
inline std::vector<uint32_t> to_v210(const std::vector<Pixel>& px, uint32_t w, uint32_t h, uint32_t row_words) {
    std::vector<uint32_t> out(static_cast<size_t>(row_words) * h, 0);
    for (uint32_t yy = 0; yy < h; ++yy)
        for (uint32_t x0 = 0, g = 0; x0 < w; x0 += 6, ++g) {
            float y[6], cb[6], cr[6];
            for (uint32_t k = 0; k < 6; ++k) ycbcr(px[yy * w + std::min(x0 + k, w - 1)], y[k], cb[k], cr[k]);
            uint32_t Y[6], CB[3], CR[3];
            for (int k = 0; k < 6; ++k) Y[k] = code10(y[k]);
            for (int k = 0; k < 3; ++k) {
                CB[k] = code10((cb[2 * k] + cb[2 * k + 1]) / 2);
                CR[k] = code10((cr[2 * k] + cr[2 * k + 1]) / 2);
            }
            uint32_t* o = &out[yy * row_words + g * 4];
            o[0] = CB[0] | (Y[0] << 10) | (CR[0] << 20);
            o[1] = Y[1] | (CB[1] << 10) | (Y[2] << 20);
            o[2] = CR[1] | (Y[3] << 10) | (CB[2] << 20);
            o[3] = Y[4] | (CR[2] << 10) | (Y[5] << 20);
        }
    return out;
}

// Row strides as DeckLink expects them.
inline uint32_t uyvy_row_words(uint32_t w) { return (w * 2 + 3) / 4; }
inline uint32_t v210_row_words(uint32_t w) { return ((w + 47) / 48) * 128 / 4; }

}  // namespace engine::convert_ref
