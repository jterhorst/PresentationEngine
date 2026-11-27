// A Core Graphics bitmap in the compositor's working format: 16-bit float,
// linear sRGB primaries, premultiplied. Drawing into it does the sRGB ->
// linear conversion, so text and images upload without another pass.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <CoreGraphics/CoreGraphics.h>

#include <cstring>
#include <string>

#include "render/seams.h"

namespace engine {

class BitmapContext {
public:
    BitmapContext(uint32_t w, uint32_t h) : w_(w), h_(h) {
        CGColorSpaceRef linear = CGColorSpaceCreateWithName(kCGColorSpaceLinearSRGB);
        ctx_ = CGBitmapContextCreate(nullptr, w, h, 16, static_cast<size_t>(w) * 8, linear,
                                     kCGImageAlphaPremultipliedLast | kCGBitmapFloatComponents |
                                         kCGBitmapByteOrder16Little);
        CGColorSpaceRelease(linear);
        if (ctx_) {
            CGContextClearRect(ctx_, CGRectMake(0, 0, w, h));
            CGContextSetShouldAntialias(ctx_, true);
            CGContextSetShouldSmoothFonts(ctx_, false);
        }
    }
    ~BitmapContext() {
        if (ctx_) CGContextRelease(ctx_);
    }
    BitmapContext(const BitmapContext&) = delete;
    BitmapContext& operator=(const BitmapContext&) = delete;

    bool ok() const { return ctx_ != nullptr; }
    CGContextRef ref() const { return ctx_; }

    void copy_to(HalfBitmap& out) const {
        out.width = w_;
        out.height = h_;
        out.pixels.resize(static_cast<size_t>(w_) * h_ * 4);
        const auto* src = static_cast<const uint8_t*>(CGBitmapContextGetData(ctx_));
        const size_t stride = CGBitmapContextGetBytesPerRow(ctx_);
        for (uint32_t y = 0; y < h_; ++y)
            std::memcpy(&out.pixels[static_cast<size_t>(y) * w_ * 4], src + y * stride, static_cast<size_t>(w_) * 8);
    }

private:
    uint32_t w_, h_;
    CGContextRef ctx_ = nullptr;
};

// ImageIO still-image decode into the working format (src/media/image_decode_apple.cpp).
bool decode_image_file(const std::string& path, HalfBitmap& out, std::string& error);

}  // namespace engine
