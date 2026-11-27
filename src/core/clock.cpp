// SPDX-License-Identifier: MPL-2.0
#include "core/clock.h"

#include <chrono>

namespace engine {

namespace {
int64_t steady_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

MonotonicClock::MonotonicClock() : origin_(steady_ns()) {}

Nanos MonotonicClock::now() const { return steady_ns() - origin_; }

Nanos FrameRate::edge(uint64_t frame) const {
    // frame * den / num seconds, in 128-bit to stay exact for years of frames.
    const __int128 n = static_cast<__int128>(frame) * den * kNanosPerSecond;
    return static_cast<Nanos>(n / num);
}

uint64_t FrameRate::frame_at(Nanos t) const {
    if (t <= 0) return 0;
    const __int128 n = static_cast<__int128>(t) * num;
    uint64_t f = static_cast<uint64_t>(n / (static_cast<__int128>(den) * kNanosPerSecond));
    // Integer division of edge() can land one frame either side; settle exactly.
    while (f > 0 && edge(f) > t) --f;
    while (edge(f + 1) <= t) ++f;
    return f;
}

}  // namespace engine
