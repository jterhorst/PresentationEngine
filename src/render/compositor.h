// Executes a RenderList through the RHI: render passes into the RGBA16F
// composite target and pooled offscreen textures, backdrop copies for blend
// modes fixed-function blending can't express, and the output conversion.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>

#include "core/resolve.h"
#include "rhi/rhi.h"

namespace engine {

constexpr uint32_t kMaxOffscreenSlots = 4;

class Compositor {
public:
    Compositor(rhi::Device& device, const OutputDesc& out);
    ~Compositor();
    Compositor(const Compositor&) = delete;
    Compositor& operator=(const Compositor&) = delete;

    // Records the frame; the result is in composite().
    void encode(const RenderList& list, rhi::CommandList& cmd);

    rhi::TextureId composite() const { return composite_; }
    const OutputDesc& output() const { return out_; }

    // Counted per frame: pooled offscreen textures this frame used.
    uint64_t take_offscreen_count();

private:
    rhi::TextureId slot(uint32_t i);
    rhi::TextureId backdrop();

    rhi::Device& device_;
    OutputDesc out_;
    rhi::TextureId composite_ = rhi::kNoTexture;
    rhi::TextureId backdrop_ = rhi::kNoTexture;
    rhi::TextureId blank_ = rhi::kNoTexture;  // bound where a draw samples nothing
    std::array<rhi::TextureId, kMaxOffscreenSlots> slots_{};
    uint64_t offscreen_used_ = 0;
};

}  // namespace engine
