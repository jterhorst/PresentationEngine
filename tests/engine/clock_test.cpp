// SPDX-License-Identifier: MPL-2.0
#include <gtest/gtest.h>

#include "core/clock.h"

using namespace engine;

TEST(FrameRate, EdgesAreExactAt5994) {
    // 60000/1001: frame 60000 starts exactly 1001 seconds in.
    EXPECT_EQ(kRate5994.edge(60000), 1001 * kNanosPerSecond);
    EXPECT_EQ(kRate5994.edge(0), 0);
    EXPECT_EQ(kRate5994.edge(1), 16683333);  // 16.683333... ms, truncated
}

TEST(FrameRate, NoDriftAfterTenHours) {
    const uint64_t frames = 10ull * 3600 * 60000 / 1001;
    const Nanos t = kRate5994.edge(frames);
    EXPECT_NEAR(static_cast<double>(t), frames * 1001.0 / 60000.0 * 1e9, 1.0);
}

TEST(FrameRate, FrameAtIsTheInverseOfEdge) {
    for (uint64_t f : {0ull, 1ull, 2ull, 59ull, 60ull, 1000ull, 123456ull}) {
        EXPECT_EQ(kRate5994.frame_at(kRate5994.edge(f)), f);
        EXPECT_EQ(kRate5994.frame_at(kRate5994.edge(f + 1) - 1), f);
    }
}

TEST(ManualClock, IsInjected) {
    ManualClock c(5);
    c.advance(ms_to_ns(10));
    EXPECT_EQ(c.now(), 5 + 10'000'000);
}
