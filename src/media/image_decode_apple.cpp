// Still images through ImageIO: the media-decode seam for stills on macOS.
// Video (AVFoundation + VideoToolbox -> CVMetalTextureCache) comes next.
//
// SPDX-License-Identifier: MPL-2.0
#include <CoreFoundation/CoreFoundation.h>
#include <ImageIO/ImageIO.h>

#include "text/bitmap_context.h"

namespace engine {

bool decode_image_file(const std::string& path, HalfBitmap& out, std::string& error) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(path.c_str()),
                                                           static_cast<CFIndex>(path.size()), false);
    CGImageSourceRef src = url ? CGImageSourceCreateWithURL(url, nullptr) : nullptr;
    if (url) CFRelease(url);
    CGImageRef image = src ? CGImageSourceCreateImageAtIndex(src, 0, nullptr) : nullptr;
    if (src) CFRelease(src);
    if (!image) {
        error = "cannot decode " + path;
        return false;
    }
    const auto w = static_cast<uint32_t>(CGImageGetWidth(image)), h = static_cast<uint32_t>(CGImageGetHeight(image));
    BitmapContext ctx(w, h);
    if (!ctx.ok()) {
        CGImageRelease(image);
        error = "bitmap context failed";
        return false;
    }
    CGContextDrawImage(ctx.ref(), CGRectMake(0, 0, w, h), image);
    CGImageRelease(image);
    ctx.copy_to(out);
    return true;
}

}  // namespace engine
