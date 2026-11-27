// SPDX-License-Identifier: MPL-2.0
#include "core/resolve.h"

#include <algorithm>
#include <cmath>

namespace engine {

float srgb_to_linear(float v) {
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

float4 to_linear_premultiplied(const v1::Color& c) {
    const float a = std::clamp(c.a(), 0.0f, 1.0f);
    return {srgb_to_linear(c.r()) * a, srgb_to_linear(c.g()) * a, srgb_to_linear(c.b()) * a, a};
}

namespace {

constexpr int32_t kSlideLayerZ = 100;

struct Ctx {
    const DocumentIndex& doc;
    const OutputDesc& out;
    const ResourceSizes& resources;
    double cw, ch;
};

TransformSpec to_spec(const v1::Transform& t) {
    TransformSpec s;
    s.position = {t.position().x(), t.position().y(), t.position().z()};
    s.size = {t.size().x(), t.size().y()};
    if (t.has_anchor()) s.anchor = {t.anchor().x(), t.anchor().y()};
    if (t.has_scale() && (t.scale().x() != 0 || t.scale().y() != 0)) s.scale = {t.scale().x(), t.scale().y()};
    s.rotation_deg = t.rotation_deg();
    s.rotation_x_deg = t.rotation_x_deg();
    s.rotation_y_deg = t.rotation_y_deg();
    return s;
}

// Grow the box by `m` on every side, keeping the pivot where it was.
void expand(TransformSpec& s, double m) {
    if (m <= 0) return;
    const double px = s.position.x + s.anchor.x * s.size.x, py = s.position.y + s.anchor.y * s.size.y;
    s.position.x -= m;
    s.position.y -= m;
    s.size.x += 2 * m;
    s.size.y += 2 * m;
    s.anchor = {(px - s.position.x) / s.size.x, (py - s.position.y) / s.size.y};
}

void set_clip(DrawParams& p, const Projected& pr, double cw, double ch) {
    for (int i = 0; i < 4; ++i) {
        const double w = pr.w[i];
        p.clip[i] = {static_cast<float>((pr.corners[i].x / cw * 2 - 1) * w),
                     static_cast<float>((1 - pr.corners[i].y / ch * 2) * w), 0, static_cast<float>(w)};
    }
}

void set_fullscreen(DrawParams& p) {
    p.clip[0] = {-1, 1, 0, 1};
    p.clip[1] = {1, 1, 0, 1};
    p.clip[2] = {1, -1, 0, 1};
    p.clip[3] = {-1, -1, 0, 1};
}

void set_common(DrawParams& p, const Ctx& ctx) {
    p.target_size = {static_cast<float>(ctx.out.width), static_cast<float>(ctx.out.height)};
    p.aa = static_cast<float>(ctx.cw / ctx.out.width);
}

void set_blend(DrawParams& p, v1::BlendMode mode, v1::BlendSpace space) {
    p.blend_mode = mode == v1::BLEND_MODE_UNSPECIFIED ? v1::BLEND_MODE_NORMAL : mode;
    p.gamma_space = space == v1::BLEND_SPACE_GAMMA ? 1 : 0;
    const bool fixed_function = p.gamma_space == 0 &&
                                (p.blend_mode == v1::BLEND_MODE_NORMAL || p.blend_mode == v1::BLEND_MODE_ADD);
    p.uses_backdrop = fixed_function ? 0 : 1;
}

// Applies ContentFit for media of natural size iw x ih in box `s`.
void apply_fit(v1::ContentFit fit, double iw, double ih, TransformSpec& s, float4& uv) {
    if (iw <= 0 || ih <= 0 || s.size.x <= 0 || s.size.y <= 0) return;
    const double box_aspect = s.size.x / s.size.y, image_aspect = iw / ih;
    switch (fit) {
    case v1::CONTENT_FIT_STRETCH: break;
    case v1::CONTENT_FIT_FIT: {
        const double w = image_aspect > box_aspect ? s.size.x : s.size.y * image_aspect;
        const double h = image_aspect > box_aspect ? s.size.x / image_aspect : s.size.y;
        s.position.x += (s.size.x - w) * s.anchor.x;
        s.position.y += (s.size.y - h) * s.anchor.y;
        s.size = {w, h};
        break;
    }
    case v1::CONTENT_FIT_NONE:
        s.position.x += (s.size.x - iw) * s.anchor.x;
        s.position.y += (s.size.y - ih) * s.anchor.y;
        s.size = {iw, ih};
        break;
    default: {  // FILL: cover the box, crop the overflow
        if (image_aspect > box_aspect) {
            const float f = static_cast<float>(box_aspect / image_aspect);
            uv = {(1 - f) / 2, 0, 1 - (1 - f) / 2, 1};
        } else {
            const float f = static_cast<float>(image_aspect / box_aspect);
            uv = {0, (1 - f) / 2, 1, 1 - (1 - f) / 2};
        }
    }
    }
}

void add_element(const Ctx& ctx, const ElementRef& ref, const ResolvedCamera& camera, RenderList& list) {
    const v1::Element& e = *ref.element;
    if (e.hidden()) return;
    DrawItem d;
    DrawParams& p = d.params;
    set_common(p, ctx);
    set_blend(p, e.blend_mode(), e.blend_space());
    p.opacity = e.opacity() == 0 ? 1.0f : std::clamp(e.opacity(), 0.0f, 1.0f);

    TransformSpec spec = to_spec(e.transform());
    p.box_size = {static_cast<float>(spec.size.x), static_cast<float>(spec.size.y)};

    if (e.has_shape()) {
        const auto& sh = e.shape();
        p.kind = static_cast<uint32_t>(sh.kind() == v1::SHAPE_KIND_ELLIPSE ? DrawKind::Ellipse : DrawKind::Rectangle);
        if (sh.has_fill()) p.fill = to_linear_premultiplied(sh.fill());
        else p.fill = {};
        p.corner_radius = sh.corner_radius();
        double margin = 0;
        if (sh.has_stroke() && sh.stroke().width() > 0) {
            p.stroke = to_linear_premultiplied(sh.stroke().color());
            p.stroke_width = sh.stroke().width();
            margin = sh.stroke().width() / 2;
        }
        for (const auto& s : sh.shadows()) {
            if (s.inner()) continue;  // inner shadows: later
            p.shadow = to_linear_premultiplied(s.color());
            p.shadow_offset = {s.offset().x(), s.offset().y()};
            p.shadow_blur = s.blur_radius();
            margin = std::max(margin, static_cast<double>(s.blur_radius()) +
                                          std::max(std::abs(s.offset().x()), std::abs(s.offset().y())));
            break;
        }
        expand(spec, std::ceil(margin));
    } else if (e.has_text()) {
        if (ref.resource == kNoResource) return;
        p.kind = static_cast<uint32_t>(DrawKind::Texture);
        d.texture0 = ref.resource;
    } else if (e.has_media()) {
        if (ref.media_index < 0) return;
        const uint32_t res = ctx.doc.media_resources[ref.media_index];
        uint32_t iw = 0, ih = 0;
        if (res == kNoResource || !ctx.resources.size_of(res, iw, ih)) return;
        p.kind = static_cast<uint32_t>(DrawKind::Texture);
        d.texture0 = res;
        const auto& mr = ctx.doc.document().media(ref.media_index);
        const double nw = mr.natural_size().width() ? mr.natural_size().width() : iw;
        const double nh = mr.natural_size().height() ? mr.natural_size().height() : ih;
        apply_fit(e.media().fit(), nw, nh, spec, p.uv_rect);
        p.box_size = {static_cast<float>(spec.size.x), static_cast<float>(spec.size.y)};
    } else {
        return;  // live input, sources and plugin elements arrive in later phases
    }
    p.quad_size = {static_cast<float>(spec.size.x), static_cast<float>(spec.size.y)};

    const Projected pr = project(spec, camera);
    if (!pr.visible) return;
    set_clip(p, pr, ctx.cw, ctx.ch);
    d.sort_depth = static_cast<float>(pr.depth);
    list.draws.push_back(d);
}

void add_container(const Ctx& ctx, const ContainerRef& c, RenderList& list) {
    const ResolvedCamera camera = resolve_camera(ctx.cw, ctx.ch, c.camera);
    const size_t start = list.draws.size();
    for (uint32_t i = 0; i < c.element_count; ++i)
        add_element(ctx, ctx.doc.elements[c.first_element + i], camera, list);
    // Back to front by center depth; insertion sort is stable and never allocates.
    auto first = list.draws.begin() + static_cast<ptrdiff_t>(start);
    for (auto it = first; it != list.draws.end(); ++it)
        std::rotate(std::upper_bound(first, it, *it,
                                     [](const DrawItem& a, const DrawItem& b) { return a.sort_depth < b.sort_depth; }),
                    it, it + 1);
}

void add_background(const Ctx& ctx, const v1::Background& bg, RenderList& list) {
    DrawItem d;
    DrawParams& p = d.params;
    set_common(p, ctx);
    set_blend(p, v1::BLEND_MODE_NORMAL, v1::BLEND_SPACE_LINEAR);
    set_fullscreen(p);
    if (bg.has_color()) {
        p.kind = static_cast<uint32_t>(DrawKind::Solid);
        p.fill = to_linear_premultiplied(bg.color());
    } else if (bg.has_media()) {
        auto m = ctx.doc.find_media(bg.media().media_id());
        if (!m) return;
        const uint32_t res = ctx.doc.media_resources[*m];
        uint32_t iw = 0, ih = 0;
        if (res == kNoResource || !ctx.resources.size_of(res, iw, ih)) return;
        p.kind = static_cast<uint32_t>(DrawKind::Texture);
        d.texture0 = res;
        TransformSpec s;
        s.size = {ctx.cw, ctx.ch};
        apply_fit(bg.media().fit(), iw, ih, s, p.uv_rect);
        if (s.size.x != ctx.cw || s.size.y != ctx.ch) {
            const Projected pr = project(s, resolve_camera(ctx.cw, ctx.ch, {}));
            set_clip(p, pr, ctx.cw, ctx.ch);
        }
    } else {
        return;  // transparent, live input and sources: later phases
    }
    list.draws.push_back(d);
}

void begin_pass(RenderList& list, int32_t target, float4 clear) {
    PassDesc pass;
    pass.target = target;
    pass.clear_color = clear;
    pass.first_draw = static_cast<uint32_t>(list.draws.size());
    list.passes.push_back(pass);
}

void end_pass(RenderList& list) {
    PassDesc& pass = list.passes.back();
    pass.draw_count = static_cast<uint32_t>(list.draws.size()) - pass.first_draw;
}

}  // namespace

void resolve(const LiveState& state, Nanos t, const OutputDesc& out, const ResourceSizes& resources,
             RenderList& list) {
    list.reset();
    if (!state.document) {
        begin_pass(list, kOutputTarget, {0, 0, 0, 1});
        end_pass(list);
        return;
    }
    const DocumentIndex& doc = *state.document;
    const Ctx ctx{doc, out, resources, doc.canvas_width(), doc.canvas_height()};

    // Fixtures sorted by layer z; fixed-size scratch, no allocation.
    std::array<uint32_t, kMaxFixtures> order{};
    const uint32_t n = state.fixture_count;
    std::copy(state.fixtures.begin(), state.fixtures.begin() + n, order.begin());
    for (uint32_t i = 1; i < n; ++i)  // insertion sort: stable, no allocation (std::stable_sort may allocate)
        for (uint32_t j = i; j > 0 && doc.fixtures[order[j]].fixture->z() < doc.fixtures[order[j - 1]].fixture->z(); --j)
            std::swap(order[j], order[j - 1]);

    const bool dissolving = state.transition_running(t);
    if (dissolving) {
        // Each side of the dissolve renders to its own pooled texture first.
        begin_pass(list, 0, {});
        if (state.previous_slide != kNone) add_container(ctx, doc.slides[state.previous_slide].container, list);
        end_pass(list);
        begin_pass(list, 1, {});
        if (state.slide != kNone) add_container(ctx, doc.slides[state.slide].container, list);
        end_pass(list);
        list.offscreen_slots = 2;
    }
    begin_pass(list, kOutputTarget, {0, 0, 0, 1});

    if (state.background) add_background(ctx, *state.background, list);
    uint32_t f = 0;
    for (; f < n && doc.fixtures[order[f]].fixture->z() < kSlideLayerZ; ++f)
        add_container(ctx, doc.fixtures[order[f]].container, list);

    if (dissolving) {
        DrawItem d;
        set_common(d.params, ctx);
        set_blend(d.params, v1::BLEND_MODE_NORMAL, v1::BLEND_SPACE_LINEAR);
        set_fullscreen(d.params);
        d.params.kind = static_cast<uint32_t>(DrawKind::Crossfade);
        d.params.mix = state.transition_progress(t);
        d.texture0 = 0;
        d.texture1 = 1;
        d.offscreen_inputs = true;
        list.draws.push_back(d);
    } else if (state.slide != kNone) {
        add_container(ctx, doc.slides[state.slide].container, list);
    }

    for (; f < n; ++f) add_container(ctx, doc.fixtures[order[f]].container, list);
    end_pass(list);
}

}  // namespace engine
