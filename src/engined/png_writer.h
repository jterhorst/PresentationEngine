// PNG in and out through ImageIO, for golden images and offline renders.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace engine {

// `bgra` is tightly packed, sRGB-encoded, opaque or straight alpha.
bool write_png_bgra(const std::string& path, const uint8_t* bgra, uint32_t w, uint32_t h);

// Reads any image ImageIO can decode as sRGB BGRA8.
bool read_png_bgra(const std::string& path, std::vector<uint8_t>& bgra, uint32_t& w, uint32_t& h);

}  // namespace engine
