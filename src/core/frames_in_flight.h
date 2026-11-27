// Frames-in-flight bookkeeping (architecture.md, "Frames in flight"): CPU work
// on frame N+1 overlaps GPU work on frame N, up to `depth` frames. Pure CPU
// state; the backend calls begin() before encoding and complete() from the GPU
// completion handler.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <cstdint>

namespace engine {

class FramesInFlight {
public:
    explicit FramesInFlight(uint32_t depth = 3) : depth_(depth) {}

    uint32_t depth() const { return depth_; }

    // Starts a frame if a slot is free. Returns false when the GPU is `depth`
    // frames behind: the caller skips this tick and records a miss, never waits.
    bool try_begin(uint64_t& frame_id);

    // Called when the GPU finishes a frame (any thread).
    void complete(uint64_t frame_id);

    uint32_t in_flight() const { return static_cast<uint32_t>(submitted_.load() - completed_.load()); }
    uint64_t completed() const { return completed_.load(); }
    uint64_t skipped() const { return skipped_; }

    // Ring slot for per-frame resources (uniform buffers, staging) of a frame.
    uint32_t slot(uint64_t frame_id) const { return static_cast<uint32_t>(frame_id % depth_); }

private:
    const uint32_t depth_;
    std::atomic<uint64_t> submitted_{0};
    std::atomic<uint64_t> completed_{0};
    uint64_t skipped_ = 0;
};

}  // namespace engine
