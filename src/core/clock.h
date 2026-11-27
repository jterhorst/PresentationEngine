// Time: an injected clock, output frame rates and frame edges.
//
// No engine code reads wall-clock time directly. Tests drive a ManualClock,
// so a ten-second transition tests in microseconds.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>

namespace engine {

using Nanos = int64_t;  // engine monotonic time, nanoseconds since engine start

constexpr Nanos kNanosPerSecond = 1'000'000'000;
constexpr Nanos ms_to_ns(int64_t ms) { return ms * 1'000'000; }

class Clock {
public:
    virtual ~Clock() = default;
    virtual Nanos now() const = 0;
};

// Monotonic time since construction.
class MonotonicClock final : public Clock {
public:
    MonotonicClock();
    Nanos now() const override;

private:
    int64_t origin_;
};

class ManualClock final : public Clock {
public:
    explicit ManualClock(Nanos start = 0) : now_(start) {}
    Nanos now() const override { return now_; }
    void set(Nanos t) { now_ = t; }
    void advance(Nanos d) { now_ += d; }

private:
    Nanos now_;
};

// An output rate such as 60000/1001 (59.94). Frame edges are exact: edge(n) is
// computed from n directly, so rounding never accumulates over a long show.
struct FrameRate {
    int32_t num = 60;
    int32_t den = 1;

    Nanos edge(uint64_t frame) const;          // start time of frame n, relative to the output's origin
    uint64_t frame_at(Nanos t) const;          // the frame whose interval contains t (t >= 0)
    Nanos frame_duration_approx() const { return edge(1); }
};

constexpr FrameRate kRate5994{60000, 1001};
constexpr FrameRate kRate60{60, 1};

}  // namespace engine
