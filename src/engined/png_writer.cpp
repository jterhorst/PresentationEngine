// SPDX-License-Identifier: MPL-2.0
#include "engined/png_writer.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

namespace engine {

namespace {
CFURLRef file_url(const std::string& path) {
    return CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(path.c_str()),
                                                   static_cast<CFIndex>(path.size()), false);
}
constexpr CGBitmapInfo kBGRA = kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little;
}  // namespace

bool write_png_bgra(const std::string& path, const uint8_t* bgra, uint32_t w, uint32_t h) {
    CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(const_cast<uint8_t*>(bgra), w, h, 8, static_cast<size_t>(w) * 4, srgb, kBGRA);
    CGImageRef image = ctx ? CGBitmapContextCreateImage(ctx) : nullptr;
    CFURLRef url = file_url(path);
    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr);
    bool ok = false;
    if (dest && image) {
        CGImageDestinationAddImage(dest, image, nullptr);
        ok = CGImageDestinationFinalize(dest);
    }
    if (dest) CFRelease(dest);
    CFRelease(url);
    if (image) CGImageRelease(image);
    if (ctx) CGContextRelease(ctx);
    CGColorSpaceRelease(srgb);
    return ok;
}

bool read_png_bgra(const std::string& path, std::vector<uint8_t>& bgra, uint32_t& w, uint32_t& h) {
    CFURLRef url = file_url(path);
    CGImageSourceRef src = CGImageSourceCreateWithURL(url, nullptr);
    CFRelease(url);
    CGImageRef image = src ? CGImageSourceCreateImageAtIndex(src, 0, nullptr) : nullptr;
    if (src) CFRelease(src);
    if (!image) return false;
    w = static_cast<uint32_t>(CGImageGetWidth(image));
    h = static_cast<uint32_t>(CGImageGetHeight(image));
    bgra.assign(static_cast<size_t>(w) * h * 4, 0);
    CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(bgra.data(), w, h, 8, static_cast<size_t>(w) * 4, srgb, kBGRA);
    CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), image);
    CGContextRelease(ctx);
    CGColorSpaceRelease(srgb);
    CGImageRelease(image);
    return true;
}

}  // namespace engine
