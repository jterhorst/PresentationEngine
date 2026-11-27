// SPDX-License-Identifier: MPL-2.0
#include "module.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

using decklink::DisplayMode;
using decklink::FrameResult;
using decklink::Module;
using decklink::PixelFormat;
using decklink::ReferenceStatus;
using decklink::Result;
using decklink::SubDevice;

namespace {

em_result to_em(Result r) {
    switch (r) {
    case Result::Ok: return EM_OK;
    case Result::Unsupported: return EM_ERR_UNSUPPORTED;
    case Result::Busy: return EM_ERR_BUSY;
    case Result::DeviceLost: return EM_ERR_DEVICE_LOST;
    case Result::InvalidArgument: return EM_ERR_INVALID_ARGUMENT;
    }
    return EM_ERR_INTERNAL;
}

bool to_format(em_pixel_format f, PixelFormat& out) {
    switch (f) {
    case EM_PIXEL_FORMAT_UYVY8: out = PixelFormat::UYVY8; return true;
    case EM_PIXEL_FORMAT_V210: out = PixelFormat::V210; return true;
    case EM_PIXEL_FORMAT_BGRA8: out = PixelFormat::BGRA8; return true;
    default: return false;  // RGBA16F never goes to SDI
    }
}

em_pixel_format to_em(PixelFormat f) {
    switch (f) {
    case PixelFormat::UYVY8: return EM_PIXEL_FORMAT_UYVY8;
    case PixelFormat::BGRA8: return EM_PIXEL_FORMAT_BGRA8;
    default: return EM_PIXEL_FORMAT_V210;
    }
}

uint32_t row_bytes(PixelFormat f, uint32_t width) {
    switch (f) {
    case PixelFormat::UYVY8: return width * 2;
    case PixelFormat::BGRA8: return width * 4;
    case PixelFormat::V210: return ((width + 47) / 48) * 128;  // 128-byte aligned groups of 48 pixels
    }
    return 0;
}

DisplayMode to_mode(const em_video_mode& m) {
    return DisplayMode{m.width, m.height, m.frame_rate.num, m.frame_rate.den, m.interlaced};
}

em_genlock_state to_genlock(ReferenceStatus s) {
    switch (s) {
    case ReferenceStatus::Locked: return EM_GENLOCK_LOCKED;
    case ReferenceStatus::Locking: return EM_GENLOCK_LOCKING;
    default: return EM_GENLOCK_NO_REFERENCE;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

struct em_output_handle final : decklink::OutputCallback {
    Module* module = nullptr;
    SubDevice* device = nullptr;
    em_output_callbacks cb{};
    em_output_open_params params{};
    PixelFormat format = PixelFormat::V210;
    DisplayMode mode;

    std::mutex mu;
    em_output_status status{};
    bool started = false;
    bool lost = false;
    uint64_t next_request = 0;        // next frame number to ask the engine for
    bool any_completed = false;
    uint64_t last_completed = 0;
    int64_t anchor_ns = 0;            // engine time of playback timeline 0
    std::vector<em_audio_buffer> pending_audio_meta;  // audio scheduled before start
    std::vector<std::vector<int32_t>> pending_audio;

    int64_t frame_ns(uint64_t frame) const {
        return static_cast<int64_t>(static_cast<__int128>(frame) * mode.rate_den * 1'000'000'000 / mode.rate_num);
    }
    int64_t now() const { return module->host()->now_ns(module->host()->host_ctx); }

    // Re-anchors engine time to the card's playback timeline. The card is the
    // master clock (genlocked or not), so display times follow its drift.
    void sample_clock() {
        const int64_t stream_ns = device->scheduled_stream_time(1'000'000'000);
        anchor_ns = now() - stream_ns;
    }

    // Requests frames to keep `preroll` scheduled ahead of what just completed.
    // Collected under the lock, delivered after it (the engine schedules from
    // inside frame_needed).
    struct Need { uint64_t frame; int64_t display, deadline; };
    void collect_needs(uint64_t completed, std::vector<Need>& out) {
        const uint64_t target = completed + params.preroll_frames;
        for (; next_request <= target; ++next_request) {
            const int64_t display = anchor_ns + frame_ns(next_request);
            out.push_back({next_request, display, display - frame_ns(1)});
        }
    }

    void frame_completed(uint64_t frame, FrameResult r) override {
        em_frame_result result = EM_FRAME_DISPLAYED;
        std::vector<Need> needs;
        bool status_change = false;
        em_output_status snapshot;
        {
            std::lock_guard lock(mu);
            switch (r) {
            case FrameResult::Completed: result = EM_FRAME_DISPLAYED; break;
            case FrameResult::DisplayedLate: result = EM_FRAME_LATE; ++status.frames_late; break;
            case FrameResult::Dropped: result = EM_FRAME_DROPPED; ++status.frames_dropped; break;
            case FrameResult::Flushed: result = EM_FRAME_FLUSHED; break;
            }
            if (r != FrameResult::Flushed && started) {
                // Slots between two completions that never got a frame repeated
                // the previous one on air: count them as dropped.
                if (any_completed && frame > last_completed + 1) status.frames_dropped += frame - last_completed - 1;
                if (!any_completed || frame > last_completed) last_completed = frame;
                any_completed = true;
                sample_clock();
                collect_needs(frame, needs);
                if (params.genlock) {
                    const em_genlock_state g = to_genlock(device->reference_status());
                    if (g != status.genlock) {
                        status.genlock = g;
                        status_change = true;
                    }
                }
            }
            snapshot = status;
        }
        cb.frame_completed(cb.engine_ctx, frame, result);
        if (status_change && cb.status_changed) cb.status_changed(cb.engine_ctx, &snapshot);
        for (const Need& n : needs) cb.frame_needed(cb.engine_ctx, n.frame, n.display, n.deadline);
    }

    void device_removed() override {
        em_output_status snapshot;
        {
            std::lock_guard lock(mu);
            lost = true;
            status.last_error = EM_ERR_DEVICE_LOST;
            snapshot = status;
        }
        module->log(EM_LOG_ERROR, std::string("device removed: ") + device->info().display_name);
        if (cb.status_changed) cb.status_changed(cb.engine_ctx, &snapshot);
    }
};

namespace {

em_result out_enumerate(void* ctx, em_device_info* out, uint32_t capacity, uint32_t* count) {
    auto* m = static_cast<Module*>(ctx);
    const auto devs = m->devices().devices();
    uint32_t n = 0;
    for (SubDevice* d : devs) {
        const auto& i = d->info();
        if (!i.can_output && !i.can_input) continue;
        if (n < capacity) {
            em_device_info& e = out[n];
            std::memset(&e, 0, sizeof e);
            std::snprintf(e.id, sizeof e.id, "%s", Module::device_id(*d).c_str());
            std::snprintf(e.name, sizeof e.name, "%s", i.display_name.c_str());
            e.connector_count = 1;
            e.caps = EM_CAP_AUDIO | (i.can_output ? EM_CAP_OUTPUT : 0) | (i.can_input ? EM_CAP_INPUT : 0) |
                     (i.reference_input ? EM_CAP_GENLOCK : 0) |
                     (i.external_key ? EM_CAP_KEY_EXTERNAL | EM_CAP_ALPHA : 0) |
                     (i.internal_key ? EM_CAP_KEY_INTERNAL : 0);
        }
        ++n;
    }
    *count = n;
    return n > capacity ? EM_ERR_BUFFER_TOO_SMALL : EM_OK;
}

em_result out_query(void*, const em_output_open_params* p, em_buffer_requirements* out) {
    PixelFormat f;
    if (!p || !out || !to_format(p->format, f)) return EM_ERR_UNSUPPORTED;
    if (p->key_mode == EM_KEY_MODE_EXTERNAL && f != PixelFormat::BGRA8) return EM_ERR_UNSUPPORTED;
    out->struct_size = sizeof *out;
    out->row_bytes = row_bytes(f, p->mode.width);
    out->size_bytes = static_cast<size_t>(out->row_bytes) * p->mode.height;
    out->alignment = 4096;
    out->key_row_bytes = 0;  // DeckLink keys from the BGRA alpha; no separate key plane
    return EM_OK;
}

em_result out_open(void* ctx, const em_output_open_params* p, const em_output_callbacks* cb, em_output_handle** out) {
    auto* m = static_cast<Module*>(ctx);
    if (!p || !cb || !out || !cb->frame_needed || !cb->frame_completed) return EM_ERR_INVALID_ARGUMENT;
    if (p->preroll_frames < 2 || p->preroll_frames > 32) return EM_ERR_INVALID_ARGUMENT;
    if (p->connector_index != 0) return EM_ERR_INVALID_ARGUMENT;  // one connector per sub-device
    SubDevice* d = m->find(p->device_id);
    if (!d) return EM_ERR_DEVICE_NOT_FOUND;
    PixelFormat f;
    if (!to_format(p->format, f)) return EM_ERR_UNSUPPORTED;
    const DisplayMode mode = to_mode(p->mode);
    if (!d->info().can_output || !d->supports(mode, f)) return EM_ERR_UNSUPPORTED;
    if (p->genlock && !d->info().reference_input) return EM_ERR_UNSUPPORTED;
    if (p->key_mode == EM_KEY_MODE_EXTERNAL && (!d->info().external_key || f != PixelFormat::BGRA8))
        return EM_ERR_UNSUPPORTED;
    if (p->key_mode == EM_KEY_MODE_INTERNAL && !d->info().internal_key) return EM_ERR_UNSUPPORTED;

    const decklink::KeyMode key = p->key_mode == EM_KEY_MODE_EXTERNAL   ? decklink::KeyMode::External
                                  : p->key_mode == EM_KEY_MODE_INTERNAL ? decklink::KeyMode::Internal
                                                                        : decklink::KeyMode::None;
    if (Result r = d->enable_video_output(mode, key); r != Result::Ok) return to_em(r);
    if (p->audio_channels) {
        if (Result r = d->enable_audio_output(p->audio_sample_rate, p->audio_channels); r != Result::Ok) {
            d->disable_output();
            return to_em(r);
        }
    }
    auto* h = new em_output_handle();
    h->module = m;
    h->device = d;
    h->cb = *cb;
    h->params = *p;
    h->params.device_id = nullptr;  // not owned; don't keep the caller's pointer
    h->format = f;
    h->mode = mode;
    h->status.struct_size = sizeof h->status;
    h->status.genlock = p->genlock ? to_genlock(d->reference_status()) : EM_GENLOCK_NOT_USED;
    h->next_request = p->preroll_frames;  // the engine schedules 0..preroll-1 before start
    d->set_output_callback(h);
    *out = h;
    return EM_OK;
}

em_result out_schedule_video(em_output_handle* h, const em_video_buffer* b) {
    if (!h || !b || !b->data) return EM_ERR_INVALID_ARGUMENT;
    {
        std::lock_guard lock(h->mu);
        if (h->lost) return EM_ERR_DEVICE_LOST;
    }
    PixelFormat f;
    if (!to_format(b->format, f) || f != h->format) return EM_ERR_INVALID_ARGUMENT;
    if (b->width != h->mode.width || b->height != h->mode.height) return EM_ERR_INVALID_ARGUMENT;
    decklink::OutputFrame frame;
    frame.data = b->data;
    frame.row_bytes = b->row_bytes;
    frame.format = f;
    frame.width = b->width;
    frame.height = b->height;
    frame.cookie = b->frame_number;
    // Display time in the mode's own timescale is exact: frame n at n * den / num.
    const int64_t timescale = h->mode.rate_num, duration = h->mode.rate_den;
    return to_em(h->device->schedule_video_frame(frame, static_cast<int64_t>(b->frame_number) * duration, duration,
                                                 timescale));
}

em_result out_schedule_audio(em_output_handle* h, const em_audio_buffer* a) {
    if (!h || !a || !a->samples || a->channel_count != h->params.audio_channels) return EM_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(h->mu);
    if (h->lost) return EM_ERR_DEVICE_LOST;
    if (!h->started) {
        // No clock anchor until start(); keep a copy (preroll, off the render loop).
        h->pending_audio_meta.push_back(*a);
        h->pending_audio.emplace_back(a->samples, a->samples + static_cast<size_t>(a->frame_count) * a->channel_count);
        return EM_OK;
    }
    const int64_t stream_ns = a->presentation_time_ns - h->anchor_ns;
    return to_em(h->device->schedule_audio_samples(a->samples, a->frame_count, stream_ns, 1'000'000'000));
}

em_result out_start(em_output_handle* h, int64_t start_time_ns) {
    if (!h) return EM_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(h->mu);
    if (h->lost) return EM_ERR_DEVICE_LOST;
    if (h->started) return EM_ERR_BUSY;
    // The engine planned frame 0 for start_time_ns: that's timeline 0.
    h->anchor_ns = start_time_ns;
    for (size_t i = 0; i < h->pending_audio.size(); ++i) {
        const auto& meta = h->pending_audio_meta[i];
        h->device->schedule_audio_samples(h->pending_audio[i].data(), meta.frame_count,
                                          meta.presentation_time_ns - h->anchor_ns, 1'000'000'000);
    }
    h->pending_audio.clear();
    h->pending_audio_meta.clear();
    if (Result r = h->device->start_scheduled_playback(0, h->mode.rate_num); r != Result::Ok) return to_em(r);
    h->started = true;
    return EM_OK;
}

em_result out_get_status(em_output_handle* h, em_output_status* out) {
    if (!h || !out) return EM_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(h->mu);
    *out = h->status;
    if (h->params.genlock && !h->lost) out->genlock = to_genlock(h->device->reference_status());
    out->buffered_frames = h->lost ? 0 : h->device->buffered_video_frames();
    return EM_OK;
}

em_result out_stop(em_output_handle* h) {
    if (!h) return EM_ERR_INVALID_ARGUMENT;
    {
        std::lock_guard lock(h->mu);
        if (h->lost) return EM_OK;
        h->started = false;
    }
    return to_em(h->device->stop_scheduled_playback());  // completions arrive as FLUSHED
}

void out_close(em_output_handle* h) {
    if (!h) return;
    h->device->set_output_callback(nullptr);
    h->device->disable_output();
    delete h;
}

}  // namespace

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

struct em_input_handle final : decklink::InputCallback {
    // Captured frames are copied into a small pool the module owns; the engine
    // hands each back through release(). A full pool drops the newest frame.
    static constexpr size_t kPool = 4;
    struct Slot {
        em_input_handle* owner = nullptr;
        std::vector<uint8_t> bytes;
        std::atomic<bool> busy{false};
    };

    Module* module = nullptr;
    SubDevice* device = nullptr;
    em_input_callbacks cb{};
    DisplayMode mode;
    PixelFormat format = PixelFormat::V210;
    std::array<Slot, kPool> pool;
    std::atomic<uint64_t> frame_number{0};
    std::atomic<uint64_t> dropped{0};

    static void release(void* ctx) { static_cast<Slot*>(ctx)->busy.store(false, std::memory_order_release); }

    void frame_arrived(const decklink::InputFrame& f) override {
        Slot* slot = nullptr;
        for (auto& s : pool) {
            bool expected = false;
            if (s.busy.compare_exchange_strong(expected, true, std::memory_order_acquire)) {
                slot = &s;
                break;
            }
        }
        if (!slot) {
            dropped.fetch_add(1);
            return;
        }
        const size_t size = static_cast<size_t>(f.row_bytes) * f.height;
        slot->bytes.resize(size);  // sized once per format; no steady-state allocation
        std::memcpy(slot->bytes.data(), f.data, size);
        em_input_frame frame{};
        frame.struct_size = sizeof frame;
        frame.video.struct_size = sizeof frame.video;
        frame.video.format = to_em(f.format);
        frame.video.width = f.width;
        frame.video.height = f.height;
        frame.video.row_bytes = f.row_bytes;
        frame.video.data = slot->bytes.data();
        frame.video.size_bytes = size;
        frame.video.frame_number = frame_number.fetch_add(1);
        frame.video.presentation_time_ns = module->host()->now_ns(module->host()->host_ctx);
        frame.release = &em_input_handle::release;
        frame.release_ctx = slot;
        cb.video_arrived(cb.engine_ctx, &frame);
    }

    void signal_lost() override {
        if (cb.signal_lost) cb.signal_lost(cb.engine_ctx);
    }

    void device_removed() override {
        module->log(EM_LOG_ERROR, std::string("device removed: ") + device->info().display_name);
        if (cb.signal_lost) cb.signal_lost(cb.engine_ctx);
    }
};

namespace {

em_result in_open(void* ctx, const em_input_open_params* p, const em_input_callbacks* cb, em_input_handle** out) {
    auto* m = static_cast<Module*>(ctx);
    if (!p || !cb || !out || !cb->video_arrived) return EM_ERR_INVALID_ARGUMENT;
    SubDevice* d = m->find(p->device_id);
    if (!d) return EM_ERR_DEVICE_NOT_FOUND;
    if (!d->info().can_input) return EM_ERR_UNSUPPORTED;
    PixelFormat f = PixelFormat::V210;
    if (p->preferred_format && !to_format(p->preferred_format, f)) return EM_ERR_UNSUPPORTED;
    // Prototype: 1080p59.94. The SDK adapter enables format detection and
    // reports changes through format_changed.
    const DisplayMode mode;
    if (Result r = d->enable_video_input(mode, f); r != Result::Ok) return to_em(r);
    auto* h = new em_input_handle();
    h->module = m;
    h->device = d;
    h->cb = *cb;
    h->mode = mode;
    h->format = f;
    for (auto& s : h->pool) s.owner = h;
    d->set_input_callback(h);
    if (cb->format_changed) {
        const em_video_mode em_mode{mode.width, mode.height, {mode.rate_num, mode.rate_den}, mode.interlaced};
        cb->format_changed(cb->engine_ctx, &em_mode, to_em(f));
    }
    *out = h;
    return EM_OK;
}

em_result in_start(em_input_handle* h) { return h ? to_em(h->device->start_streams()) : EM_ERR_INVALID_ARGUMENT; }
em_result in_stop(em_input_handle* h) { return h ? to_em(h->device->stop_streams()) : EM_ERR_INVALID_ARGUMENT; }

void in_close(em_input_handle* h) {
    if (!h) return;
    h->device->set_input_callback(nullptr);
    h->device->disable_input();
    delete h;
}

}  // namespace

// ---------------------------------------------------------------------------
// Module
// ---------------------------------------------------------------------------

namespace decklink {

Module::Module(const em_host_api* host, std::string module_id, std::unique_ptr<DeviceProvider> devices)
    : host_(host), id_(std::move(module_id)), devices_(std::move(devices)) {}

Module::~Module() = default;

std::string Module::device_id(const SubDevice& d) { return "decklink:" + d.info().persistent_id; }

SubDevice* Module::find(const char* em_device_id) {
    if (!em_device_id) return nullptr;
    for (SubDevice* d : devices_->devices())
        if (device_id(*d) == em_device_id) return d;
    return nullptr;
}

void Module::log(em_log_level level, const std::string& message) const {
    if (host_ && host_->log) host_->log(host_->host_ctx, level, id_.c_str(), message.c_str());
}

em_result Module::register_providers() {
    output_.struct_size = sizeof output_;
    output_.kind = "sdi";
    output_.provider_ctx = this;
    output_.enumerate = out_enumerate;
    output_.query_buffer_requirements = out_query;
    output_.open = out_open;
    output_.start = out_start;
    output_.schedule_video = out_schedule_video;
    output_.schedule_audio = out_schedule_audio;
    output_.get_status = out_get_status;
    output_.stop = out_stop;
    output_.close = out_close;
    if (em_result r = host_->register_output_provider(host_->host_ctx, &output_); r != EM_OK) return r;

    input_.struct_size = sizeof input_;
    input_.kind = "sdi";
    input_.provider_ctx = this;
    input_.enumerate = out_enumerate;  // same sub-devices; caps say which can capture
    input_.open = in_open;
    input_.start = in_start;
    input_.stop = in_stop;
    input_.close = in_close;
    return host_->register_input_provider(host_->host_ctx, &input_);
}

}  // namespace decklink
