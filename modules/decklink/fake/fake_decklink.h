// FakeDeckLink: a simulated DeckLink card behind the module's device seam
// (architecture.md, "Testing": simulated DeckLink).
//
// Time is injected. advance() moves the host clock; each card's clock follows
// at its oscillator's rate (crystal error, or the reference's when genlocked),
// so ten minutes of 59.94 playback, drift and lock/loss tests run in
// milliseconds. Outputs can be wired back into inputs to check bytes end to
// end. It can't reproduce driver, firmware or real-signal timing; that's what
// the weekly hardware loop is for.
//
// Callbacks fire on the thread that calls advance(), never while the fake
// holds its lock.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "device.h"

namespace decklink::fake {

struct ReferenceSignal {
    bool present = false;
    double ppm = 0;  // house sync's rate error against the host clock
};

// What an output put on the wire for one frame slot.
struct ShownFrame {
    int64_t slot = 0;           // frame index on the playback timeline
    uint64_t cookie = 0;        // engine frame number; repeats keep the previous one
    bool repeat = false;        // underrun: nothing scheduled, the last frame repeated
    FrameResult result = FrameResult::Completed;
};

struct AudioRecord {
    int64_t stream_time_ns = 0;
    std::vector<int32_t> samples;  // interleaved, exactly as scheduled
};

struct CardSpec {
    std::string model = "DeckLink Duo 2";
    std::string persistent_id = "fake-duo2-0001";
    uint32_t sub_devices = 4;
    bool reference_input = true;
    // Fill + key pairs (fill index, key index). Duo 2 pairs connectors 1+2 and 3+4;
    // confirm per connector pair in the SDK before promising alpha out (open item).
    std::vector<std::pair<uint32_t, uint32_t>> key_pairs = {{0, 1}, {2, 3}};
    uint32_t max_height = 1080;  // 3G-SDI: up to 1080p60
    int64_t lock_time_ns = 500'000'000;  // reference present -> locked
};

class FakeDeckLink final : public DeviceProvider {
public:
    explicit FakeDeckLink(CardSpec spec = {});
    ~FakeDeckLink() override;

    std::vector<SubDevice*> devices() override;

    // ---- Injected time ----
    int64_t host_now() const;
    void advance(int64_t host_ns);  // fires completions, loopback and input frames due in that span

    // ---- Signal environment ----
    void set_reference(ReferenceSignal ref);
    void set_crystal_ppm(double ppm);  // the card's free-running oscillator error

    // ---- Wiring and faults ----
    void loopback(uint32_t output_index, uint32_t input_index);  // SDI cable from an output to an input
    void unplug();                                               // the card disappears (Thunderbolt pulled)

    // ---- Inspection ----
    std::vector<ShownFrame> shown(uint32_t sub_device) const;
    std::vector<AudioRecord> audio(uint32_t sub_device) const;
    uint64_t underruns(uint32_t sub_device) const;
    std::vector<uint8_t> last_key_plane(uint32_t sub_device) const;  // alpha of the last keyed frame, row-packed

    struct Sub;  // one simulated sub-device

private:
    friend struct Sub;
    struct Event;
    void step(int64_t dt, std::vector<Event>& events);
    void deliver_loopback(uint32_t from, const OutputFrame& f, int64_t time_ns, const DisplayMode& mode,
                          std::vector<Event>& events);
    void fire(std::vector<Event>& events);
    ReferenceStatus reference_status_locked() const;
    double clock_rate_locked() const;

    CardSpec spec_;
    mutable std::mutex mu_;
    int64_t host_ns_ = 0;
    double card_ns_ = 0;                 // the card's own clock
    ReferenceSignal reference_;
    int64_t reference_since_ = 0;
    double crystal_ppm_ = 0;
    bool unplugged_ = false;
    std::vector<std::unique_ptr<Sub>> subs_;
    std::map<uint32_t, uint32_t> cables_;  // output -> input
};

}  // namespace decklink::fake
