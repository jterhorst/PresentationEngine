// Resolve: walk the live layer stack into a flat render list for one output
// (architecture.md, step 2). Reuses the list's arrays, so after the first few
// frames it never allocates.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <vector>

#include "core/clock.h"
#include "core/draw_params.h"
#include "core/live_state.h"

namespace engine {

struct OutputDesc {
    uint32_t width = 1920, height = 1080;  // target pixels
};

constexpr int32_t kOutputTarget = -1;

struct DrawItem {
    DrawParams params;
    uint32_t texture0 = kNoResource;   // resource id, or an offscreen slot for crossfades
    uint32_t texture1 = kNoResource;
    bool offscreen_inputs = false;     // texture0/1 name offscreen slots, not resources
    float sort_depth = 0;
};

struct PassDesc {
    int32_t target = kOutputTarget;    // kOutputTarget or an offscreen slot
    float4 clear_color;                // linear premultiplied
    uint32_t first_draw = 0;
    uint32_t draw_count = 0;
};

struct RenderList {
    std::vector<PassDesc> passes;
    std::vector<DrawItem> draws;
    uint32_t offscreen_slots = 0;      // slots this frame needs

    void reset() {
        passes.clear();  // keeps capacity
        draws.clear();
        offscreen_slots = 0;
    }
};

// Color conversion from the protocol's sRGB straight alpha to the compositor's
// linear premultiplied.
float4 to_linear_premultiplied(const v1::Color& c);
float srgb_to_linear(float v);

// Resource lookup for resolve: text and media textures were made at load time.
struct ResourceSizes {
    virtual ~ResourceSizes() = default;
    virtual bool size_of(uint32_t resource, uint32_t& w, uint32_t& h) const = 0;
};

void resolve(const LiveState& state, Nanos t, const OutputDesc& out, const ResourceSizes& resources,
             RenderList& list);

}  // namespace engine
