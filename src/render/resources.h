// Prepares a loaded document's GPU resources before any of it goes live:
// text rasterized through the text seam, stills decoded through the media
// seam, all uploaded as RGBA16F textures. Runs on the control thread, never
// on the render thread (principle 1: the frame never waits on disk).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>
#include <vector>

#include "core/document_index.h"
#include "core/resolve.h"
#include "render/seams.h"
#include "rhi/rhi.h"

namespace engine {

struct PrepareReport {
    uint32_t text_textures = 0;
    uint32_t media_textures = 0;
    uint64_t bytes_uploaded = 0;
    std::vector<std::string> warnings;
};

// `scale` is output pixels per canvas unit, so text is rasterized at the
// largest size any output shows it.
PrepareReport prepare_resources(DocumentIndex& doc, rhi::Device& device, TextRasterizer& text,
                                ImageDecoder& images, double scale);

void release_resources(DocumentIndex& doc, rhi::Device& device);

// Lets resolve() ask the device for texture sizes.
class DeviceResourceSizes final : public ResourceSizes {
public:
    explicit DeviceResourceSizes(const rhi::Device& device) : device_(device) {}
    bool size_of(uint32_t resource, uint32_t& w, uint32_t& h) const override {
        return device_.texture_size(resource, w, h);
    }

private:
    const rhi::Device& device_;
};

}  // namespace engine
