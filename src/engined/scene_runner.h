// Runs a reference scene: loads and prepares its document, feeds its script
// through the command queue at frame edges, and renders frames. Used by
// `engined` (preview window, offline renders, counter runs) and the GPU tests.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/reference_scene.h"
#include "core/resolve.h"
#include "render/compositor.h"
#include "render/resources.h"

namespace engine {

class SceneRunner {
public:
    static std::unique_ptr<SceneRunner> load(const std::string& path, rhi::Device& device, TextRasterizer& text,
                                             ImageDecoder& images, std::string& error);
    ~SceneRunner();

    const testing_v1::ReferenceScene& scene() const { return scene_; }
    const PrepareReport& prepare_report() const { return report_; }
    FrameRate rate() const { return rate_; }
    Nanos duration() const { return ms_to_ns(scene_.duration_ms() ? scene_.duration_ms() : 5000); }
    LiveState& state() { return state_; }
    Compositor& compositor() { return *compositor_; }
    const DocumentIndex& document() const { return *doc_; }

    // Back to time zero: document loaded, nothing live, script rewound.
    void restart();

    // Applies every script step due by `t`, each at its own frame edge.
    void advance_script(Nanos t);

    // Control-side command (the preview's keyboard): validated now, applied at
    // the next frame edge.
    bool send(const v1::Command& command, std::string& error);
    // Next/previous slide in document order, with the presentation's transition.
    bool step_slide(int delta);

    // Render-thread work for one frame at `t`: apply queued commands at the
    // edge, resolve, encode into `cmd`. Adds this frame's counters.
    void render(Nanos t, rhi::CommandList& cmd, FrameCounters& counters);

    // Offline: render the frame at `t` and read it back as BGRA8 (sRGB).
    bool render_bgra(Nanos t, std::vector<uint8_t>& bgra, FrameCounters* counters = nullptr);

    // Deterministic counter run: plays the scene twice at its output rate and
    // returns totals for the second (steady-state) pass.
    FrameCounters measure(uint64_t& frames);

private:
    SceneRunner(rhi::Device& device) : device_(device) {}
    Nanos edge_at_or_after(Nanos t) const;

    rhi::Device& device_;
    testing_v1::ReferenceScene scene_;
    std::unique_ptr<DocumentIndex> doc_;
    std::unique_ptr<Compositor> compositor_;
    std::unique_ptr<DeviceResourceSizes> sizes_;
    PrepareReport report_;
    FrameRate rate_;
    OutputDesc out_;
    LiveState state_;
    CommandQueue queue_;
    RenderList list_;
    int next_step_ = 0;
    rhi::TextureId readback_ = rhi::kNoTexture;
};

}  // namespace engine
