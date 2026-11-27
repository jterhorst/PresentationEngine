// Deterministic per-frame counters: the first layer of the performance gate
// (architecture.md, "Performance budget and regression gating"). They never
// flake, so CI compares them with perf/baseline.json on every PR.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>

namespace engine {

struct FrameCounters {
    uint64_t allocations = 0;      // engine heap allocations on the render thread; must be 0 in steady state
    uint64_t driver_allocations = 0; // made inside the GPU driver while encoding; gated by baseline, not zero
    uint64_t lock_waits = 0;       // contended locks on the render thread; must be 0 in steady state
    uint64_t render_passes = 0;
    uint64_t draws = 0;
    uint64_t backdrop_copies = 0;  // target copies for blend modes fixed-function blending can't do
    uint64_t offscreen_textures = 0;
    uint64_t bytes_uploaded = 0;
    uint64_t shaders_compiled = 0;

    FrameCounters& operator+=(const FrameCounters& o) {
        allocations += o.allocations;
        driver_allocations += o.driver_allocations;
        lock_waits += o.lock_waits;
        render_passes += o.render_passes;
        draws += o.draws;
        backdrop_copies += o.backdrop_copies;
        offscreen_textures += o.offscreen_textures;
        bytes_uploaded += o.bytes_uploaded;
        shaders_compiled += o.shaders_compiled;
        return *this;
    }
};

// Allocation tracking. Only counts when the alloc_tracking object library is
// linked into the executable (it replaces global operator new); otherwise
// tracking_available() is false and counts stay 0.
namespace alloc_tracking {
bool available();
uint64_t thread_allocations();         // engine allocations on the calling thread since it started
uint64_t thread_driver_allocations();  // allocations made while a DriverScope was open
void enter_driver();
void leave_driver();
}  // namespace alloc_tracking

// Marks calls into a platform driver (Metal encoder creation allocates
// inside the driver). Those allocations are counted separately, so the zero
// gate measures what the engine controls.
class DriverScope {
public:
    DriverScope() { alloc_tracking::enter_driver(); }
    ~DriverScope() { alloc_tracking::leave_driver(); }
    DriverScope(const DriverScope&) = delete;
    DriverScope& operator=(const DriverScope&) = delete;
};

// Counts allocations on the current thread over a scope.
class AllocationScope {
public:
    AllocationScope()
        : start_(alloc_tracking::thread_allocations()), driver_start_(alloc_tracking::thread_driver_allocations()) {}
    uint64_t count() const { return alloc_tracking::thread_allocations() - start_; }
    uint64_t driver_count() const { return alloc_tracking::thread_driver_allocations() - driver_start_; }

private:
    uint64_t start_;
    uint64_t driver_start_;
};

}  // namespace engine
