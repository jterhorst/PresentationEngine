// Replaces global operator new to count heap allocations per thread. Linked
// into engined and engine_tests only (see CMakeLists.txt).
//
// SPDX-License-Identifier: MPL-2.0
#include <cstdint>
#include <cstdlib>
#include <new>

#include "core/counters.h"

namespace {
thread_local uint64_t t_allocations = 0;
thread_local uint64_t t_driver_allocations = 0;
thread_local int t_driver_depth = 0;

inline void count() {
    if (t_driver_depth > 0) ++t_driver_allocations;
    else ++t_allocations;
}

void* counted_alloc(std::size_t size) {
    count();
    if (size == 0) size = 1;
    if (void* p = std::malloc(size)) return p;
    throw std::bad_alloc();
}

void* counted_aligned_alloc(std::size_t size, std::align_val_t align) {
    count();
    void* p = nullptr;
    if (posix_memalign(&p, static_cast<std::size_t>(align), size ? size : 1) != 0) throw std::bad_alloc();
    return p;
}
}  // namespace

namespace engine::alloc_tracking {
bool available() { return true; }
uint64_t thread_allocations() { return t_allocations; }
uint64_t thread_driver_allocations() { return t_driver_allocations; }
void enter_driver() { ++t_driver_depth; }
void leave_driver() { --t_driver_depth; }
}  // namespace engine::alloc_tracking

void* operator new(std::size_t size) { return counted_alloc(size); }
void* operator new[](std::size_t size) { return counted_alloc(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    count();
    return std::malloc(size ? size : 1);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    count();
    return std::malloc(size ? size : 1);
}
void* operator new(std::size_t size, std::align_val_t a) { return counted_aligned_alloc(size, a); }
void* operator new[](std::size_t size, std::align_val_t a) { return counted_aligned_alloc(size, a); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
