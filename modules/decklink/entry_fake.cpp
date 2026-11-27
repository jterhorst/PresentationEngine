// core.decklink-fake: the DeckLink module driving a FakeDeckLink Duo 2 in
// real time. Loads like a real card, so engined and module hosts can run the
// whole output path (and the nightly soak) with no hardware.
//
// ENGINE_DECKLINK_FAKE_LOOPBACK="0>1" cables sub-device 0's output into 1's input.
//
// SPDX-License-Identifier: MPL-2.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "fake/fake_decklink.h"
#include "module.h"

namespace {

struct FakeModule {
    decklink::fake::FakeDeckLink* card = nullptr;  // owned by module
    std::unique_ptr<decklink::Module> module;
    std::atomic<bool> running{true};
    std::thread driver;
};

// Advances the fake by wall-clock time, the way a real card's clock runs.
void drive(FakeModule* f) {
    using clock = std::chrono::steady_clock;
    auto last = clock::now();
    while (f->running.load()) {
        std::this_thread::sleep_for(std::chrono::microseconds(500));
        const auto now = clock::now();
        f->card->advance(std::chrono::duration_cast<std::chrono::nanoseconds>(now - last).count());
        last = now;
    }
}

em_result init(const em_host_api* host, void** ctx) {
    auto* f = new FakeModule();
    auto card = std::make_unique<decklink::fake::FakeDeckLink>();
    f->card = card.get();
    if (const char* cable = std::getenv("ENGINE_DECKLINK_FAKE_LOOPBACK")) {
        unsigned out = 0, in = 0;
        if (std::sscanf(cable, "%u>%u", &out, &in) == 2) f->card->loopback(out, in);
    }
    f->module = std::make_unique<decklink::Module>(host, "core.decklink-fake", std::move(card));
    f->driver = std::thread(drive, f);
    *ctx = f;
    return f->module->register_providers();
}

void shutdown(void* ctx) {
    auto* f = static_cast<FakeModule*>(ctx);
    f->running.store(false);
    f->driver.join();
    delete f;
}

}  // namespace

extern "C" EM_EXPORT const em_module_descriptor* em_module_entry(void) {
    static const em_module_descriptor d = {EM_ABI_VERSION, sizeof(em_module_descriptor), "core.decklink-fake",
                                           "DeckLink (simulated Duo 2)", "0.1.0", "MPL-2.0", init, shutdown};
    return &d;
}
