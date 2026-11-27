// Commands apply atomically at frame edges, never mid-frame, without allocating.
// SPDX-License-Identifier: MPL-2.0
#include <gtest/gtest.h>

#include <thread>

#include "core/counters.h"
#include "core/live_state.h"
#include "test_documents.h"

using namespace engine;

TEST(SpscQueue, FifoAndBounded) {
    SpscQueue<int, 4> q;
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(q.push(i));
    EXPECT_FALSE(q.push(99)) << "a full queue rejects; the engine replies BUSY";
    int v = -1;
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(q.pop(v));
        EXPECT_EQ(v, i);
    }
    EXPECT_FALSE(q.pop(v));
}

TEST(SpscQueue, CrossThreadOrderIsPreserved) {
    SpscQueue<int, 64> q;
    constexpr int kCount = 100000;
    std::thread producer([&] {
        for (int i = 0; i < kCount; ++i)
            while (!q.push(i)) std::this_thread::yield();
    });
    int expected = 0, v;
    while (expected < kCount)
        if (q.pop(v)) ASSERT_EQ(v, expected++);
    producer.join();
}

TEST(LiveState, CommandsApplyOnlyAtTheFrameEdge) {
    auto doc = test_docs::three_slides();
    CommandQueue queue;
    LiveState state;
    ASSERT_TRUE(state.apply({LiveCommand::Kind::LoadDocument, doc.get()}, 0));

    LiveCommand take;
    take.kind = LiveCommand::Kind::TakeSlide;
    take.index = 1;
    ASSERT_TRUE(queue.push(take));
    EXPECT_EQ(state.slide, kNone) << "queued, not applied mid-frame";

    const Nanos edge = kRate5994.edge(3);
    EXPECT_EQ(state.apply_pending(queue, edge), 1u);
    EXPECT_EQ(state.slide, 1u);
}

TEST(LiveState, ApplyingCommandsDoesNotAllocate) {
    auto doc = test_docs::three_slides();
    CommandQueue queue;
    LiveState state;
    state.apply({LiveCommand::Kind::LoadDocument, doc.get()}, 0);
    LiveCommand take;
    take.kind = LiveCommand::Kind::TakeSlide;
    for (uint32_t i = 0; i < 3; ++i) {
        take.index = i;
        queue.push(take);
    }
    AllocationScope scope;
    state.apply_pending(queue, kRate5994.edge(10));
    EXPECT_EQ(scope.count(), 0u);
}

TEST(LiveState, DissolveRunsForItsDurationOnTheInjectedClock) {
    auto doc = test_docs::three_slides(/*dissolve_ms=*/1000);
    LiveState state;
    state.apply({LiveCommand::Kind::LoadDocument, doc.get()}, 0);
    LiveCommand take;
    take.kind = LiveCommand::Kind::TakeSlide;
    take.index = 0;
    state.apply(take, 0);
    take.index = 1;
    const Nanos start = ms_to_ns(2000);
    state.apply(take, start);

    EXPECT_TRUE(state.transition_running(start));
    EXPECT_EQ(state.previous_slide, 0u);
    EXPECT_FLOAT_EQ(state.transition_progress(start + ms_to_ns(250)), 0.25f);
    EXPECT_FALSE(state.transition_running(start + ms_to_ns(1000)));
    EXPECT_FLOAT_EQ(state.transition_progress(start + ms_to_ns(1000)), 1.0f);
}

TEST(LiveState, SlideActionsShowFixturesAndSetTheBackground) {
    auto doc = test_docs::three_slides();
    LiveState state;
    state.apply({LiveCommand::Kind::LoadDocument, doc.get()}, 0);
    LiveCommand take;
    take.kind = LiveCommand::Kind::TakeSlide;
    take.index = 2;  // has a show_fixture action and a background
    state.apply(take, 0);
    EXPECT_EQ(state.fixture_count, 1u);
    ASSERT_NE(state.background, nullptr);
    EXPECT_TRUE(state.background->has_color());

    LiveCommand clear;
    clear.kind = LiveCommand::Kind::ClearLayer;
    clear.layer = v1::LAYER_TARGET_ALL;
    state.apply(clear, 0);
    EXPECT_EQ(state.fixture_count, 0u);
    EXPECT_EQ(state.slide, kNone);
    EXPECT_EQ(state.background, nullptr);
}
