// The shared text library, part 2 on macOS: lay out StyledText with Core Text
// inside the element's container (minus exclusions) and rasterize it into a
// linear-light RGBA16F bitmap. The native editor links this same code so its
// line breaks match the output exactly.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "render/seams.h"

namespace engine {

class CoreTextRasterizer final : public TextRasterizer {
public:
    bool rasterize(const v1::TextElement& text, double box_w, double box_h, double scale, HalfBitmap& out,
                   std::string& error) override;
};

class ImageIODecoder final : public ImageDecoder {
public:
    bool decode(const std::string& path, HalfBitmap& out, std::string& error) override;
};

}  // namespace engine
