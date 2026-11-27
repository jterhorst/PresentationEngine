// SPDX-License-Identifier: MPL-2.0
#include "core/frames_in_flight.h"

namespace engine {

bool FramesInFlight::try_begin(uint64_t& frame_id) {
    const uint64_t submitted = submitted_.load(std::memory_order_relaxed);
    if (submitted - completed_.load(std::memory_order_acquire) >= depth_) {
        ++skipped_;
        return false;
    }
    frame_id = submitted;
    submitted_.store(submitted + 1, std::memory_order_release);
    return true;
}

void FramesInFlight::complete(uint64_t) {
    // Metal completes command buffers in submission order on one queue, so a
    // count is enough; the id is kept for backends that complete out of order.
    completed_.fetch_add(1, std::memory_order_acq_rel);
}

}  // namespace engine
