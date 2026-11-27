// SPDX-License-Identifier: MPL-2.0
#include <gtest/gtest.h>

#include "core/frames_in_flight.h"

using engine::FramesInFlight;

TEST(FramesInFlight, NeverMoreThanDepthAndNeverWaits) {
    FramesInFlight f(2);
    uint64_t a, b, c;
    ASSERT_TRUE(f.try_begin(a));
    ASSERT_TRUE(f.try_begin(b));
    EXPECT_FALSE(f.try_begin(c)) << "GPU two frames behind: skip, record the miss";
    EXPECT_EQ(f.skipped(), 1u);
    f.complete(a);
    ASSERT_TRUE(f.try_begin(c));
    EXPECT_EQ(f.in_flight(), 2u);
}

TEST(FramesInFlight, SlotsCycleThroughTheRing) {
    FramesInFlight f(3);
    uint64_t id;
    for (uint32_t i = 0; i < 7; ++i) {
        ASSERT_TRUE(f.try_begin(id));
        EXPECT_EQ(f.slot(id), i % 3);
        f.complete(id);
    }
}
