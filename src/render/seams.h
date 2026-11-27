// Platform seams the compositor's resource preparation uses: text layout and
// still-image decode. macOS implementations: Core Text and ImageIO.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "engine/v1/document.pb.h"

namespace engine {

// Linear-light, premultiplied RGBA as IEEE half floats: the compositor's
// working format, uploaded without conversion.
struct HalfBitmap {
    uint32_t width = 0, height = 0;
    std::vector<uint16_t> pixels;  // width * height * 4
    size_t bytes_per_row() const { return static_cast<size_t>(width) * 8; }
};

class TextRasterizer {
public:
    virtual ~TextRasterizer() = default;
    // Lays out `text` in a box of box_w x box_h canvas units and rasterizes it
    // at `scale` pixels per canvas unit.
    virtual bool rasterize(const v1::TextElement& text, double box_w, double box_h, double scale,
                           HalfBitmap& out, std::string& error) = 0;
};

class ImageDecoder {
public:
    virtual ~ImageDecoder() = default;
    virtual bool decode(const std::string& path, HalfBitmap& out, std::string& error) = 0;
};

}  // namespace engine
