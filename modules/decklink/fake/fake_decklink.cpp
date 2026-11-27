// SPDX-License-Identifier: MPL-2.0
#include "fake/fake_decklink.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace decklink::fake {

namespace {

constexpr int64_t kStepNs = 1'000'000;  // simulation step; slots are found exactly within it

int64_t slot_ns(const DisplayMode& m, int64_t slot) {
    return static_cast<int64_t>(static_cast<__int128>(slot) * m.rate_den * 1'000'000'000 / m.rate_num);
}

int64_t to_ns(int64_t t, int64_t timescale) {
    return static_cast<int64_t>(static_cast<__int128>(t) * 1'000'000'000 / timescale);
}

uint32_t bytes_per_row(PixelFormat f, uint32_t width) {
    switch (f) {
    case PixelFormat::UYVY8: return width * 2;
    case PixelFormat::BGRA8: return width * 4;
    case PixelFormat::V210: return ((width + 47) / 48) * 128;
    }
    return 0;
}

}  // namespace

struct FakeDeckLink::Event {
    enum class Kind { Completed, OutputRemoved, InputFrame, InputRemoved, SignalLost } kind;
    OutputCallback* out = nullptr;
    InputCallback* in = nullptr;
    uint64_t cookie = 0;
    FrameResult result = FrameResult::Completed;
    InputFrame frame;
    std::vector<uint8_t> bytes;
};

struct FakeDeckLink::Sub final : SubDevice {
    struct Scheduled {
        OutputFrame frame;
        bool late = false;  // scheduled after its slot had already started
    };

    FakeDeckLink& card;
    uint32_t index;
    SubDeviceInfo info_;

    // Output
    bool output_enabled = false;
    DisplayMode out_mode;
    KeyMode key = KeyMode::None;
    bool key_partner_busy = false;  // this connector carries another's key plane
    uint32_t audio_channels = 0;
    OutputCallback* out_cb = nullptr;
    std::map<int64_t, Scheduled> scheduled;  // by slot
    bool playing = false;
    int64_t start_timeline_ns = 0;
    double card_at_start = 0;
    int64_t next_slot = 0;
    OutputFrame last_shown;
    bool have_last = false;
    uint64_t underruns = 0;
    std::vector<ShownFrame> shown;
    std::vector<AudioRecord> audio;
    std::vector<uint8_t> key_plane;

    // Input
    bool input_enabled = false;
    bool streaming = false;
    DisplayMode in_mode;
    PixelFormat in_format = PixelFormat::V210;
    InputCallback* in_cb = nullptr;
    bool signal_reported_lost = false;

    Sub(FakeDeckLink& c, uint32_t i) : card(c), index(i) {
        info_.persistent_id = c.spec_.persistent_id + ":" + std::to_string(i);
        info_.display_name = "DeckLink Duo (" + std::to_string(i + 1) + ")";
        info_.model = c.spec_.model;
        info_.can_output = info_.can_input = true;
        info_.reference_input = c.spec_.reference_input;
        for (const auto& [fill, k] : c.spec_.key_pairs)
            if (fill == i) info_.external_key = true;
    }

    const SubDeviceInfo& info() const override { return info_; }

    bool supports(const DisplayMode& m, PixelFormat) const override {
        if (m.rate_num <= 0 || m.rate_den <= 0) return false;
        if (m.height > card.spec_.max_height) return false;
        const double fps = double(m.rate_num) / m.rate_den;
        static const double rates[] = {23.976, 24, 25, 29.97, 30, 50, 59.94, 60};
        for (double r : rates)
            if (std::abs(fps - r) < 0.01) return !(m.interlaced && fps > 30.01);
        return false;
    }

    Result enable_video_output(const DisplayMode& m, KeyMode k) override {
        std::lock_guard lock(card.mu_);
        if (card.unplugged_) return Result::DeviceLost;
        if (output_enabled || input_enabled || key_partner_busy) return Result::Busy;
        if (!supports(m, PixelFormat::V210)) return Result::Unsupported;
        if (k == KeyMode::Internal) return Result::Unsupported;  // the Duo 2 keys externally only
        if (k == KeyMode::External) {
            Sub* partner = nullptr;
            for (const auto& [fill, key_idx] : card.spec_.key_pairs)
                if (fill == index) partner = card.subs_[key_idx].get();
            if (!partner) return Result::Unsupported;
            if (partner->output_enabled || partner->input_enabled || partner->key_partner_busy) return Result::Busy;
            partner->key_partner_busy = true;
        }
        output_enabled = true;
        out_mode = m;
        key = k;
        return Result::Ok;
    }

    Result enable_audio_output(uint32_t rate, uint32_t channels) override {
        std::lock_guard lock(card.mu_);
        if (card.unplugged_) return Result::DeviceLost;
        if (!output_enabled) return Result::InvalidArgument;
        if (rate != 48000 || (channels != 2 && channels != 8 && channels != 16)) return Result::Unsupported;
        audio_channels = channels;
        return Result::Ok;
    }

    void set_output_callback(OutputCallback* cb) override {
        std::lock_guard lock(card.mu_);
        out_cb = cb;
    }

    int64_t timeline_ns_locked() const {
        if (!playing) return start_timeline_ns;
        return start_timeline_ns + static_cast<int64_t>(card.card_ns_ - card_at_start);
    }

    Result schedule_video_frame(const OutputFrame& f, int64_t display_time, int64_t duration,
                                int64_t timescale) override {
        std::lock_guard lock(card.mu_);
        if (card.unplugged_) return Result::DeviceLost;
        if (!output_enabled || timescale <= 0 || duration <= 0 || !f.data) return Result::InvalidArgument;
        if (f.width != out_mode.width || f.height != out_mode.height) return Result::InvalidArgument;
        if (f.row_bytes < bytes_per_row(f.format, f.width)) return Result::InvalidArgument;
        if (key == KeyMode::External && f.format != PixelFormat::BGRA8) return Result::InvalidArgument;
        const int64_t ns = to_ns(display_time, timescale);
        // Slot whose display time this is (rounded: timescales needn't divide evenly).
        const int64_t slot = static_cast<int64_t>(
            (static_cast<__int128>(ns) * out_mode.rate_num + static_cast<__int128>(out_mode.rate_den) * 500'000'000) /
            (static_cast<__int128>(out_mode.rate_den) * 1'000'000'000));
        if (scheduled.size() >= 64) return Result::Busy;  // the card's frame queue is finite
        Scheduled s{f, playing && slot < next_slot};
        scheduled[slot] = s;
        return Result::Ok;
    }

    Result schedule_audio_samples(const int32_t* samples, uint32_t frames, int64_t stream_time,
                                  int64_t timescale) override {
        std::lock_guard lock(card.mu_);
        if (card.unplugged_) return Result::DeviceLost;
        if (!audio_channels || !samples || timescale <= 0) return Result::InvalidArgument;
        AudioRecord r;
        r.stream_time_ns = to_ns(stream_time, timescale);
        r.samples.assign(samples, samples + static_cast<size_t>(frames) * audio_channels);
        audio.push_back(std::move(r));
        return Result::Ok;
    }

    Result start_scheduled_playback(int64_t start_time, int64_t timescale) override {
        std::lock_guard lock(card.mu_);
        if (card.unplugged_) return Result::DeviceLost;
        if (!output_enabled || playing || timescale <= 0) return Result::InvalidArgument;
        playing = true;
        start_timeline_ns = to_ns(start_time, timescale);
        card_at_start = card.card_ns_;
        next_slot = (static_cast<__int128>(start_timeline_ns) * out_mode.rate_num + out_mode.rate_den * 1'000'000'000LL - 1) /
                    (static_cast<__int128>(out_mode.rate_den) * 1'000'000'000);
        return Result::Ok;
    }

    Result stop_scheduled_playback() override {
        std::vector<Event> events;
        {
            std::lock_guard lock(card.mu_);
            if (card.unplugged_) return Result::DeviceLost;
            playing = false;
            for (const auto& [slot, s] : scheduled)
                events.push_back({Event::Kind::Completed, out_cb, nullptr, s.frame.cookie, FrameResult::Flushed});
            scheduled.clear();
        }
        card.fire(events);
        return Result::Ok;
    }

    Result disable_output() override {
        std::lock_guard lock(card.mu_);
        if (key == KeyMode::External)
            for (const auto& [fill, key_idx] : card.spec_.key_pairs)
                if (fill == index) card.subs_[key_idx]->key_partner_busy = false;
        output_enabled = playing = false;
        key = KeyMode::None;
        scheduled.clear();
        return card.unplugged_ ? Result::DeviceLost : Result::Ok;
    }

    uint32_t buffered_video_frames() const override {
        std::lock_guard lock(card.mu_);
        uint32_t n = 0;
        for (const auto& [slot, s] : scheduled) n += (!playing || slot >= next_slot) ? 1 : 0;
        return n;
    }

    int64_t scheduled_stream_time(int64_t timescale) const override {
        std::lock_guard lock(card.mu_);
        return static_cast<int64_t>(static_cast<__int128>(timeline_ns_locked()) * timescale / 1'000'000'000);
    }

    ReferenceStatus reference_status() const override {
        std::lock_guard lock(card.mu_);
        return card.reference_status_locked();
    }

    Result enable_video_input(const DisplayMode& m, PixelFormat f) override {
        std::lock_guard lock(card.mu_);
        if (card.unplugged_) return Result::DeviceLost;
        if (output_enabled || input_enabled || key_partner_busy) return Result::Busy;
        if (!supports(m, f)) return Result::Unsupported;
        input_enabled = true;
        in_mode = m;
        in_format = f;
        return Result::Ok;
    }

    void set_input_callback(InputCallback* cb) override {
        std::lock_guard lock(card.mu_);
        in_cb = cb;
    }

    Result start_streams() override {
        std::lock_guard lock(card.mu_);
        if (card.unplugged_) return Result::DeviceLost;
        if (!input_enabled) return Result::InvalidArgument;
        streaming = true;
        return Result::Ok;
    }

    Result stop_streams() override {
        std::lock_guard lock(card.mu_);
        streaming = false;
        return card.unplugged_ ? Result::DeviceLost : Result::Ok;
    }

    Result disable_input() override {
        std::lock_guard lock(card.mu_);
        input_enabled = streaming = false;
        return card.unplugged_ ? Result::DeviceLost : Result::Ok;
    }

    // Puts slots due by the current card time on the wire. Called with the lock held.
    void tick(std::vector<Event>& events) {
        if (!playing) return;
        // Frames scheduled for slots already gone were never shown.
        for (auto it = scheduled.begin(); it != scheduled.end() && it->first < next_slot;) {
            events.push_back({Event::Kind::Completed, out_cb, nullptr, it->second.frame.cookie, FrameResult::Dropped});
            it = scheduled.erase(it);
        }
        const int64_t now = timeline_ns_locked();
        while (slot_ns(out_mode, next_slot) <= now) {
            ShownFrame s;
            s.slot = next_slot;
            auto it = scheduled.find(next_slot);
            if (it != scheduled.end() && !it->second.late) {
                last_shown = it->second.frame;
                have_last = true;
                s.cookie = last_shown.cookie;
                events.push_back({Event::Kind::Completed, out_cb, nullptr, s.cookie, FrameResult::Completed});
                if (key == KeyMode::External) {  // the pair connector carries the alpha as key
                    key_plane.resize(static_cast<size_t>(last_shown.width) * last_shown.height);
                    const auto* px = static_cast<const uint8_t*>(last_shown.data);
                    for (uint32_t y = 0; y < last_shown.height; ++y)
                        for (uint32_t x = 0; x < last_shown.width; ++x)
                            key_plane[static_cast<size_t>(y) * last_shown.width + x] =
                                px[static_cast<size_t>(y) * last_shown.row_bytes + x * 4 + 3];
                }
                scheduled.erase(it);
            } else {
                if (it != scheduled.end()) {
                    events.push_back({Event::Kind::Completed, out_cb, nullptr, it->second.frame.cookie, FrameResult::Dropped});
                    scheduled.erase(it);
                }
                s.repeat = true;
                s.cookie = have_last ? last_shown.cookie : 0;
                ++underruns;
            }
            shown.push_back(s);
            if (have_last) card.deliver_loopback(index, last_shown, slot_ns(out_mode, next_slot), out_mode, events);
            ++next_slot;
        }
    }
};

// Copies what output `from` just put on the wire into the cabled input, if any.
void FakeDeckLink::deliver_loopback(uint32_t from, const OutputFrame& f, int64_t time_ns, const DisplayMode& mode,
                                    std::vector<Event>& events) {
    auto cable = cables_.find(from);
    if (cable == cables_.end()) return;
    Sub& in = *subs_[cable->second];
    if (!in.streaming || !in.in_cb) return;
    if (!(in.in_mode == mode) || in.in_format != f.format) {
        if (!in.signal_reported_lost) events.push_back({Event::Kind::SignalLost, nullptr, in.in_cb});
        in.signal_reported_lost = true;
        return;
    }
    in.signal_reported_lost = false;
    Event e{Event::Kind::InputFrame, nullptr, in.in_cb};
    const uint32_t row = bytes_per_row(f.format, f.width);
    e.bytes.resize(static_cast<size_t>(row) * f.height);
    for (uint32_t y = 0; y < f.height; ++y)
        std::memcpy(&e.bytes[static_cast<size_t>(y) * row], static_cast<const uint8_t*>(f.data) + static_cast<size_t>(y) * f.row_bytes, row);
    e.frame.row_bytes = row;
    e.frame.format = f.format;
    e.frame.width = f.width;
    e.frame.height = f.height;
    e.frame.timescale = mode.rate_num;
    e.frame.stream_time = static_cast<int64_t>(static_cast<__int128>(time_ns) * mode.rate_num / 1'000'000'000);
    events.push_back(std::move(e));
}

FakeDeckLink::FakeDeckLink(CardSpec spec) : spec_(std::move(spec)) {
    for (uint32_t i = 0; i < spec_.sub_devices; ++i) subs_.push_back(std::make_unique<Sub>(*this, i));
}

FakeDeckLink::~FakeDeckLink() = default;

std::vector<SubDevice*> FakeDeckLink::devices() {
    std::lock_guard lock(mu_);
    std::vector<SubDevice*> out;
    if (unplugged_) return out;
    for (auto& s : subs_) out.push_back(s.get());
    return out;
}

int64_t FakeDeckLink::host_now() const {
    std::lock_guard lock(mu_);
    return host_ns_;
}

ReferenceStatus FakeDeckLink::reference_status_locked() const {
    if (!spec_.reference_input) return ReferenceStatus::NotSupported;
    if (!reference_.present) return ReferenceStatus::NoReference;
    return host_ns_ - reference_since_ < spec_.lock_time_ns ? ReferenceStatus::Locking : ReferenceStatus::Locked;
}

double FakeDeckLink::clock_rate_locked() const {
    const double ppm = reference_status_locked() == ReferenceStatus::Locked ? reference_.ppm : crystal_ppm_;
    return 1.0 + ppm * 1e-6;
}

void FakeDeckLink::step(int64_t dt, std::vector<Event>& events) {
    card_ns_ += dt * clock_rate_locked();
    host_ns_ += dt;
    for (auto& s : subs_) s->tick(events);
}

void FakeDeckLink::advance(int64_t host_ns) {
    while (host_ns > 0) {
        std::vector<Event> events;
        {
            std::lock_guard lock(mu_);
            if (unplugged_) {
                host_ns_ += host_ns;
                return;
            }
            const int64_t dt = std::min(host_ns, kStepNs);
            step(dt, events);
            host_ns -= dt;
        }
        fire(events);
    }
}

void FakeDeckLink::fire(std::vector<Event>& events) {
    for (auto& e : events) {
        switch (e.kind) {
        case Event::Kind::Completed:
            if (e.out) e.out->frame_completed(e.cookie, e.result);
            break;
        case Event::Kind::OutputRemoved:
            if (e.out) e.out->device_removed();
            break;
        case Event::Kind::InputFrame:
            e.frame.data = e.bytes.data();
            e.in->frame_arrived(e.frame);
            break;
        case Event::Kind::InputRemoved:
            e.in->device_removed();
            break;
        case Event::Kind::SignalLost:
            e.in->signal_lost();
            break;
        }
    }
}

void FakeDeckLink::set_reference(ReferenceSignal ref) {
    std::lock_guard lock(mu_);
    if (ref.present && !reference_.present) reference_since_ = host_ns_;
    reference_ = ref;
}

void FakeDeckLink::set_crystal_ppm(double ppm) {
    std::lock_guard lock(mu_);
    crystal_ppm_ = ppm;
}

void FakeDeckLink::loopback(uint32_t output_index, uint32_t input_index) {
    std::lock_guard lock(mu_);
    cables_[output_index] = input_index;
}

void FakeDeckLink::unplug() {
    std::vector<Event> events;
    {
        std::lock_guard lock(mu_);
        unplugged_ = true;
        for (auto& s : subs_) {
            if (s->output_enabled && s->out_cb) events.push_back({Event::Kind::OutputRemoved, s->out_cb});
            if (s->input_enabled && s->in_cb) events.push_back({Event::Kind::InputRemoved, nullptr, s->in_cb});
            s->playing = s->streaming = false;
        }
    }
    fire(events);
}

std::vector<ShownFrame> FakeDeckLink::shown(uint32_t i) const {
    std::lock_guard lock(mu_);
    return subs_[i]->shown;
}

std::vector<AudioRecord> FakeDeckLink::audio(uint32_t i) const {
    std::lock_guard lock(mu_);
    return subs_[i]->audio;
}

uint64_t FakeDeckLink::underruns(uint32_t i) const {
    std::lock_guard lock(mu_);
    return subs_[i]->underruns;
}

std::vector<uint8_t> FakeDeckLink::last_key_plane(uint32_t i) const {
    std::lock_guard lock(mu_);
    return subs_[i]->key_plane;
}

}  // namespace decklink::fake
