// SPDX-License-Identifier: MPL-2.0
#include "render/compositor.h"

namespace engine {

Compositor::Compositor(rhi::Device& device, const OutputDesc& out) : device_(device), out_(out) {
    slots_.fill(rhi::kNoTexture);
    rhi::TextureDesc d;
    d.width = out.width;
    d.height = out.height;
    d.render_target = true;
    d.cpu_readable = true;
    d.label = "composite";
    composite_ = device_.create_texture(d);

    rhi::TextureDesc b;
    b.width = b.height = 1;
    b.label = "blank";
    blank_ = device_.create_texture(b);
    const uint16_t zero[4] = {0, 0, 0, 0};
    device_.upload_texture(blank_, zero, 8);
}

Compositor::~Compositor() {
    for (auto id : {composite_, backdrop_, blank_})
        if (id != rhi::kNoTexture) device_.destroy_texture(id);
    for (auto id : slots_)
        if (id != rhi::kNoTexture) device_.destroy_texture(id);
}

rhi::TextureId Compositor::slot(uint32_t i) {
    if (i >= kMaxOffscreenSlots) return blank_;
    if (slots_[i] == rhi::kNoTexture) {
        // Pooled: created the first time a frame needs it, then reused. A
        // feature that's off never creates one.
        rhi::TextureDesc d;
        d.width = out_.width;
        d.height = out_.height;
        d.render_target = true;
        d.label = "offscreen";
        slots_[i] = device_.create_texture(d);
    }
    return slots_[i];
}

rhi::TextureId Compositor::backdrop() {
    if (backdrop_ == rhi::kNoTexture) {
        rhi::TextureDesc d;
        d.width = out_.width;
        d.height = out_.height;
        d.label = "backdrop";
        backdrop_ = device_.create_texture(d);
    }
    return backdrop_;
}

uint64_t Compositor::take_offscreen_count() {
    const uint64_t n = offscreen_used_;
    offscreen_used_ = 0;
    return n;
}

void Compositor::encode(const RenderList& list, rhi::CommandList& cmd) {
    offscreen_used_ += list.offscreen_slots;
    for (const PassDesc& pass : list.passes) {
        const rhi::TextureId target = pass.target == kOutputTarget ? composite_ : slot(static_cast<uint32_t>(pass.target));
        cmd.begin_render_pass(target, &pass.clear_color);
        for (uint32_t i = 0; i < pass.draw_count; ++i) {
            const DrawItem& d = list.draws[pass.first_draw + i];
            rhi::TextureId t0 = blank_, t1 = blank_;
            if (d.offscreen_inputs) {
                t0 = slot(d.texture0);
                t1 = slot(d.texture1);
            } else if (d.texture0 != kNoResource) {
                t0 = d.texture0;
            }
            if (d.params.uses_backdrop) {
                // Blend modes beyond NORMAL and ADD read what's already in the
                // target: copy it, then draw with blending off.
                cmd.end_render_pass();
                cmd.copy_texture(target, backdrop());
                cmd.begin_render_pass(target, nullptr);
                cmd.draw_quad(d.params, t0, backdrop(), rhi::Blend::Replace);
            } else {
                const rhi::Blend blend =
                    d.params.blend_mode == v1::BLEND_MODE_ADD ? rhi::Blend::Add : rhi::Blend::PremultipliedOver;
                cmd.draw_quad(d.params, t0, t1, blend);
            }
        }
        cmd.end_render_pass();
    }
}

}  // namespace engine
