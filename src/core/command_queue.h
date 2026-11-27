// Commands cross from the control thread to the render thread through a
// fixed-capacity single-producer, single-consumer ring. Pushing never blocks
// the renderer and popping never allocates or takes a lock.
//
// Commands are applied only at frame edges (see LiveState::apply), never mid-frame.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace engine {

template <class T, size_t Capacity>
class SpscQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
    // Producer side. Returns false when full; the caller reports BUSY.
    bool push(const T& value) {
        const size_t head = head_.load(std::memory_order_relaxed);
        if (head - tail_.load(std::memory_order_acquire) == Capacity) return false;
        slots_[head & (Capacity - 1)] = value;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Consumer side.
    bool pop(T& out) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        out = slots_[tail & (Capacity - 1)];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    size_t size_approx() const {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
    }

private:
    std::array<T, Capacity> slots_{};
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

}  // namespace engine
