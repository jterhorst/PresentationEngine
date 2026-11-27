// Scene resolve: z sorting, layer stacking, transitions, and no per-frame allocation.
// SPDX-License-Identifier: MPL-2.0
#include <gtest/gtest.h>

#include "core/counters.h"
#include "core/resolve.h"
#include "test_documents.h"

using namespace engine;

namespace {

struct NoResources final : ResourceSizes {
    bool size_of(uint32_t, uint32_t&, uint32_t&) const override { return false; }
};

LiveState live(const DocumentIndex& doc, uint32_t slide) {
    LiveState s;
    s.apply({LiveCommand::Kind::LoadDocument, &doc}, 0);
    LiveCommand take;
    take.kind = LiveCommand::Kind::TakeSlide;
    take.index = slide;
    s.apply(take, 0);
    return s;
}

}  // namespace

TEST(Resolve, ElementsDrawBackToFrontByDepth) {
    auto doc = test_docs::three_slides();
    RenderList list;
    resolve(live(*doc, 0), 0, {}, NoResources{}, list);
    ASSERT_EQ(list.passes.size(), 1u);
    ASSERT_EQ(list.draws.size(), 3u);
    // Elements a (z 0), b (z 50), c (z -50) draw c, a, b.
    EXPECT_FLOAT_EQ(list.draws[0].sort_depth, -50);
    EXPECT_FLOAT_EQ(list.draws[1].sort_depth, 0);
    EXPECT_FLOAT_EQ(list.draws[2].sort_depth, 50);
}

TEST(Resolve, EqualDepthKeepsListOrder) {
    auto doc = test_docs::three_slides();
    v1::Document copy = doc->document();
    auto* s = copy.mutable_presentations(0)->mutable_slides(0);
    for (auto& e : *s->mutable_elements()) e.mutable_transform()->mutable_position()->set_z(0);
    s->mutable_elements(0)->mutable_shape()->mutable_fill()->set_g(0.25f);
    s->mutable_elements(2)->mutable_shape()->mutable_fill()->set_g(0.75f);
    DocumentIndex idx(std::move(copy));
    RenderList list;
    resolve(live(idx, 0), 0, {}, NoResources{}, list);
    ASSERT_EQ(list.draws.size(), 3u);
    EXPECT_GT(list.draws[2].params.fill.y, list.draws[0].params.fill.y) << "ties keep list order";
}

TEST(Resolve, LayersStackBackgroundThenSlideThenFixtures) {
    auto doc = test_docs::three_slides();
    RenderList list;
    resolve(live(*doc, 2), 0, {}, NoResources{}, list);
    ASSERT_EQ(list.draws.size(), 5u);  // background + 3 slide elements + fixture
    EXPECT_EQ(list.draws.front().params.kind, static_cast<uint32_t>(DrawKind::Solid));
    EXPECT_FLOAT_EQ(list.draws.back().params.box_size.x, 1920) << "fixture z 150 draws above the slide";
}

TEST(Resolve, ZeroDepthElementMapsToItsBox) {
    auto doc = test_docs::three_slides();
    RenderList list;
    resolve(live(*doc, 0), 0, {}, NoResources{}, list);
    const DrawParams& p = list.draws[1].params;  // element a at (100,100) 200x100, z 0
    EXPECT_NEAR(p.clip[0].x, 100.0 / 1920 * 2 - 1, 1e-6);
    EXPECT_NEAR(p.clip[0].y, 1 - 100.0 / 1080 * 2, 1e-6);
    EXPECT_NEAR(p.clip[2].x, 300.0 / 1920 * 2 - 1, 1e-6);
    EXPECT_FLOAT_EQ(p.clip[0].w, 1);
}

TEST(Resolve, DissolveRendersBothSidesOffscreenThenCrossfades) {
    auto doc = test_docs::three_slides(1000);
    LiveState s = live(*doc, 0);
    LiveCommand take;
    take.kind = LiveCommand::Kind::TakeSlide;
    take.index = 1;
    s.apply(take, ms_to_ns(100));
    RenderList list;
    resolve(s, ms_to_ns(600), {}, NoResources{}, list);
    ASSERT_EQ(list.passes.size(), 3u);
    EXPECT_EQ(list.passes[0].target, 0);
    EXPECT_EQ(list.passes[1].target, 1);
    EXPECT_EQ(list.passes[2].target, kOutputTarget);
    EXPECT_EQ(list.offscreen_slots, 2u);
    const DrawItem& fade = list.draws.back();
    EXPECT_EQ(fade.params.kind, static_cast<uint32_t>(DrawKind::Crossfade));
    EXPECT_NEAR(fade.params.mix, 0.5f, 1e-6);

    resolve(s, ms_to_ns(1200), {}, NoResources{}, list);
    EXPECT_EQ(list.passes.size(), 1u) << "after the transition: straight to the output, no offscreen cost";
}

TEST(Resolve, SteadyStateDoesNotAllocate) {
    auto doc = test_docs::three_slides(1000);
    LiveState s = live(*doc, 2);
    RenderList list;
    resolve(s, 0, {}, NoResources{}, list);  // warm-up sizes the arrays
    LiveCommand take;
    take.kind = LiveCommand::Kind::TakeSlide;
    take.index = 1;
    s.apply(take, ms_to_ns(10));
    resolve(s, ms_to_ns(20), {}, NoResources{}, list);  // a dissolve frame sizes the pass list

    AllocationScope scope;
    for (int f = 0; f < 120; ++f) resolve(s, ms_to_ns(f * 16), {}, NoResources{}, list);
    EXPECT_EQ(scope.count(), 0u);
}

TEST(Resolve, BlendModesFixedFunctionCannotDoUseTheBackdrop) {
    v1::Document copy = test_docs::three_slides()->document();
    auto* els = copy.mutable_presentations(0)->mutable_slides(0)->mutable_elements();
    els->Mutable(0)->set_blend_mode(v1::BLEND_MODE_ADD);
    els->Mutable(1)->set_blend_mode(v1::BLEND_MODE_MULTIPLY);
    els->Mutable(2)->set_blend_mode(v1::BLEND_MODE_NORMAL);
    els->Mutable(2)->set_blend_space(v1::BLEND_SPACE_GAMMA);
    DocumentIndex idx(std::move(copy));
    RenderList list;
    resolve(live(idx, 0), 0, {}, NoResources{}, list);
    // Sorted c (gamma normal), a (add), b (multiply).
    EXPECT_EQ(list.draws[0].params.uses_backdrop, 1u);
    EXPECT_EQ(list.draws[1].params.uses_backdrop, 0u);
    EXPECT_EQ(list.draws[2].params.uses_backdrop, 1u);
}
