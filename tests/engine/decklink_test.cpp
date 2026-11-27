// The DeckLink module against FakeDeckLink (architecture.md, "Simulated
// DeckLink"): scheduling and buffering, v210 and audio byte for byte, A/V
// sync, Duo 2 connector mapping, genlock lock/loss/drift, device removal and
// unsupported modes. Injected time: ten minutes of 59.94 runs in about a second.
//
// SPDX-License-Identifier: MPL-2.0
#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <vector>

#include "fake/fake_decklink.h"
#include "module.h"
#include "render/convert_reference.h"

using decklink::fake::CardSpec;
using decklink::fake::FakeDeckLink;
using decklink::fake::ReferenceSignal;

namespace {

constexpr int64_t kSecond = 1'000'000'000;
constexpr uint64_t kTenMinutes5994 = 35964;  // 600 s * 60000 / 1001, rounded down

int64_t frame_ns(uint64_t f) { return static_cast<int64_t>(static_cast<__int128>(f) * 1001 * kSecond / 60000); }

// A minimal engine: owns output buffers, answers frame_needed by scheduling
// (unless told to miss), records what comes back.
struct Harness {
    FakeDeckLink* card;
    std::unique_ptr<decklink::Module> module;
    em_host_api host{};
    em_output_provider out{};
    em_input_provider in{};
    em_output_handle* handle = nullptr;
    em_output_open_params params{};
    em_buffer_requirements req{};
    std::vector<std::vector<uint8_t>> pool;  // 8 frame buffers
    std::set<uint64_t> skip;                 // frames the "engine" misses
    std::map<uint64_t, em_frame_result> completed;
    std::vector<em_output_status> status_changes;
    std::map<uint64_t, int64_t> display_times;  // from frame_needed
    bool with_audio = false;
    bool full_fill = false;  // write every byte (loopback/key checks); soaks only stamp the frame number
    std::map<uint64_t, std::vector<int32_t>> audio_sent;

    explicit Harness(CardSpec spec = {}) {
        auto owned = std::make_unique<FakeDeckLink>(spec);
        card = owned.get();
        host.abi_version = EM_ABI_VERSION;
        host.struct_size = sizeof host;
        host.host_ctx = this;
        host.now_ns = [](void* ctx) { return static_cast<Harness*>(ctx)->card->host_now(); };
        host.log = [](void*, em_log_level, const char*, const char*) {};
        host.register_output_provider = [](void* ctx, const em_output_provider* p) {
            static_cast<Harness*>(ctx)->out = *p;
            return EM_OK;
        };
        host.register_input_provider = [](void* ctx, const em_input_provider* p) {
            static_cast<Harness*>(ctx)->in = *p;
            return EM_OK;
        };
        host.devices_changed = [](void*, const char*) {};
        module = std::make_unique<decklink::Module>(&host, "core.decklink-test", std::move(owned));
        EXPECT_EQ(module->register_providers(), EM_OK);
    }

    ~Harness() {
        if (handle) {
            out.stop(handle);
            out.close(handle);
        }
    }

    static std::string id(uint32_t sub) { return "decklink:fake-duo2-0001:" + std::to_string(sub); }

    em_output_open_params default_params(uint32_t sub, em_pixel_format f = EM_PIXEL_FORMAT_V210) {
        static std::string ids[8];
        ids[sub] = id(sub);
        em_output_open_params p{};
        p.struct_size = sizeof p;
        p.device_id = ids[sub].c_str();
        p.mode = {1920, 1080, {60000, 1001}, false};
        p.format = f;
        p.preroll_frames = 3;
        return p;
    }

    void fill(uint64_t frame, uint8_t* data, bool full) {
        if (!full) {
            std::memcpy(data, &frame, sizeof frame);
            return;
        }
        // Distinct per frame so loopback checks catch any mix-up.
        for (size_t i = 0; i < req.size_bytes; i += 4) {
            const uint32_t w = static_cast<uint32_t>(frame * 2654435761u + i * 40503u) & 0x3FFFFFFF;
            std::memcpy(data + i, &w, 4);
        }
    }

    void schedule(uint64_t frame) {
        if (skip.count(frame)) return;
        uint8_t* data = pool[frame % pool.size()].data();
        fill(frame, data, full_fill);
        em_video_buffer b{};
        b.struct_size = sizeof b;
        b.format = params.format;
        b.width = params.mode.width;
        b.height = params.mode.height;
        b.row_bytes = req.row_bytes;
        b.data = data;
        b.size_bytes = req.size_bytes;
        b.frame_number = frame;
        ASSERT_EQ(out.schedule_video(handle, &b), EM_OK) << "frame " << frame;
        if (with_audio) send_audio(frame);
    }

    // 48 kHz at 59.94: 800.8 samples per frame, as 801/801/801/801/800.
    void send_audio(uint64_t frame) {
        const uint32_t n = (frame % 5 == 4) ? 800 : 801;
        auto& s = audio_sent[frame];
        s.resize(n * params.audio_channels);
        for (size_t i = 0; i < s.size(); ++i) s[i] = static_cast<int32_t>((frame << 16) ^ (i * 7919));
        em_audio_buffer a{};
        a.struct_size = sizeof a;
        a.samples = s.data();
        a.channel_count = params.audio_channels;
        a.frame_count = n;
        a.sample_rate = 48000;
        a.presentation_time_ns = display_times.count(frame) ? display_times[frame] : frame_ns(frame);
        ASSERT_EQ(out.schedule_audio(handle, &a), EM_OK);
    }

    em_result open(const em_output_open_params& p) {
        params = p;
        if (em_result r = out.query_buffer_requirements(out.provider_ctx, &p, &req); r != EM_OK) return r;
        pool.assign(8, std::vector<uint8_t>(req.size_bytes));
        em_output_callbacks cb{};
        cb.struct_size = sizeof cb;
        cb.engine_ctx = this;
        cb.frame_needed = [](void* ctx, uint64_t frame, int64_t display, int64_t) {
            auto* h = static_cast<Harness*>(ctx);
            h->display_times[frame] = display;
            h->schedule(frame);
        };
        cb.frame_completed = [](void* ctx, uint64_t frame, em_frame_result r) {
            static_cast<Harness*>(ctx)->completed[frame] = r;
        };
        cb.status_changed = [](void* ctx, const em_output_status* s) {
            static_cast<Harness*>(ctx)->status_changes.push_back(*s);
        };
        return out.open(out.provider_ctx, &p, &cb, &handle);
    }

    void start() {
        for (uint64_t f = 0; f < params.preroll_frames; ++f) schedule(f);  // preroll
        ASSERT_EQ(out.start(handle, card->host_now()), EM_OK);
    }

    em_output_status status() {
        em_output_status s{};
        out.get_status(handle, &s);
        return s;
    }

    size_t count(em_frame_result r) const {
        size_t n = 0;
        for (const auto& [f, res] : completed) n += res == r;
        return n;
    }
};

}  // namespace

TEST(DeckLink, EnumeratesTheDuo2AsFourSubDevices) {
    Harness h;
    em_device_info devs[8];
    uint32_t n = 0;
    ASSERT_EQ(h.out.enumerate(h.out.provider_ctx, devs, 8, &n), EM_OK);
    ASSERT_EQ(n, 4u);
    for (uint32_t i = 0; i < 4; ++i) {
        EXPECT_EQ(devs[i].id, Harness::id(i));
        EXPECT_EQ(std::string(devs[i].name), "DeckLink Duo (" + std::to_string(i + 1) + ")");
        EXPECT_TRUE(devs[i].caps & EM_CAP_OUTPUT);
        EXPECT_TRUE(devs[i].caps & EM_CAP_INPUT);
        EXPECT_TRUE(devs[i].caps & EM_CAP_GENLOCK) << "the reference input serves every connector";
        EXPECT_EQ(bool(devs[i].caps & EM_CAP_KEY_EXTERNAL), i == 0 || i == 2) << "fill side of pairs 1+2, 3+4";
    }
    EXPECT_EQ(h.out.enumerate(h.out.provider_ctx, devs, 2, &n), EM_ERR_BUFFER_TOO_SMALL);
    EXPECT_EQ(n, 4u);
}

TEST(DeckLink, RejectsWhatTheCardCannotDo) {
    {
        Harness h;
        auto p = h.default_params(0);
        p.mode = {3840, 2160, {60000, 1001}, false};
        EXPECT_EQ(h.open(p), EM_ERR_UNSUPPORTED) << "2160p needs 12G-SDI; the Duo 2 is 3G";
    }
    {
        Harness h;
        auto p = h.default_params(0);
        em_buffer_requirements req;
        p.format = EM_PIXEL_FORMAT_RGBA16F;
        EXPECT_EQ(h.out.query_buffer_requirements(h.out.provider_ctx, &p, &req), EM_ERR_UNSUPPORTED);
    }
    {
        Harness h;
        auto p = h.default_params(0);
        p.mode = {1920, 1080, {30000, 1001}, true};
        EXPECT_EQ(h.open(p), EM_OK) << "1080i59.94";
    }
    {
        CardSpec spec;
        spec.reference_input = false;
        Harness h(spec);
        auto p = h.default_params(0);
        p.genlock = true;
        EXPECT_EQ(h.open(p), EM_ERR_UNSUPPORTED) << "no reference input";
    }
    {
        Harness h;
        auto p = h.default_params(0);
        p.device_id = "decklink:nope";
        EXPECT_EQ(h.open(p), EM_ERR_DEVICE_NOT_FOUND);
    }
}

TEST(DeckLink, TenMinutesAt5994WithoutADrop) {
    Harness h;
    ASSERT_EQ(h.open(h.default_params(0)), EM_OK);
    h.start();
    h.card->advance(600 * kSecond);

    const auto shown = h.card->shown(0);
    ASSERT_GE(shown.size(), kTenMinutes5994);
    for (size_t i = 0; i < shown.size(); ++i) {
        ASSERT_FALSE(shown[i].repeat) << "underrun at slot " << i;
        ASSERT_EQ(shown[i].cookie, i) << "frames on air in order";
    }
    EXPECT_EQ(h.card->underruns(0), 0u);
    const auto s = h.status();
    EXPECT_EQ(s.frames_dropped, 0u);
    EXPECT_EQ(s.frames_late, 0u);
    EXPECT_EQ(s.buffered_frames, h.params.preroll_frames) << "preroll depth holds";
    EXPECT_EQ(h.count(EM_FRAME_DISPLAYED), shown.size());
    // Display times the engine was given land on exact 59.94 edges.
    // (Frames 0-2 are preroll; the first frame_needed is frame 3.)
    EXPECT_EQ(h.display_times.at(30003) - h.display_times.at(3), frame_ns(30000));
}

TEST(DeckLink, AMissedFrameRepeatsTheLastAndIsCounted) {
    Harness h;
    h.skip = {100};
    ASSERT_EQ(h.open(h.default_params(0)), EM_OK);
    h.start();
    h.card->advance(5 * kSecond);

    const auto shown = h.card->shown(0);
    ASSERT_GT(shown.size(), 200u);
    EXPECT_TRUE(shown[100].repeat);
    EXPECT_EQ(shown[100].cookie, 99u) << "the card repeats frame 99 on air";
    EXPECT_FALSE(shown[101].repeat) << "and recovers on the next";
    EXPECT_EQ(h.card->underruns(0), 1u);
    EXPECT_EQ(h.status().frames_dropped, 1u);
}

TEST(DeckLink, AFrameScheduledAfterItsSlotIsDropped) {
    Harness h;
    h.skip = {50};
    ASSERT_EQ(h.open(h.default_params(0)), EM_OK);
    h.start();
    h.card->advance(2 * kSecond);  // slot 50 is long gone
    h.skip.clear();
    h.schedule(50);
    h.card->advance(20'000'000);
    ASSERT_TRUE(h.completed.count(50));
    EXPECT_EQ(h.completed.at(50), EM_FRAME_DROPPED);
}

TEST(DeckLink, V210LoopbackIsByteExact) {
    Harness h;
    h.full_fill = true;
    h.card->loopback(0, 1);

    struct Capture {
        std::vector<std::vector<uint8_t>> frames;
    } capture;
    em_input_callbacks icb{};
    icb.struct_size = sizeof icb;
    icb.engine_ctx = &capture;
    icb.video_arrived = [](void* ctx, const em_input_frame* f) {
        auto* c = static_cast<Capture*>(ctx);
        const auto* p = static_cast<const uint8_t*>(f->video.data);
        c->frames.emplace_back(p, p + f->video.size_bytes);
        f->release(f->release_ctx);
    };
    em_input_open_params ip{};
    ip.struct_size = sizeof ip;
    const std::string in_id = Harness::id(1);
    ip.device_id = in_id.c_str();
    ip.preferred_format = EM_PIXEL_FORMAT_V210;
    em_input_handle* input = nullptr;
    ASSERT_EQ(h.in.open(h.in.provider_ctx, &ip, &icb, &input), EM_OK);
    ASSERT_EQ(h.in.start(input), EM_OK);

    ASSERT_EQ(h.open(h.default_params(0)), EM_OK);
    EXPECT_EQ(h.req.row_bytes, engine::convert_ref::v210_row_words(1920) * 4);
    h.start();
    h.card->advance(2 * kSecond);

    ASSERT_GE(capture.frames.size(), 110u);
    std::vector<uint8_t> want(h.req.size_bytes);
    for (size_t k = 0; k < capture.frames.size(); ++k) {
        h.fill(k, want.data(), true);
        ASSERT_EQ(capture.frames[k], want) << "frame " << k << " changed on the wire";
    }
    h.in.stop(input);
    h.in.close(input);
}

TEST(DeckLink, AudioLandsWithItsVideoFrame) {
    Harness h;
    h.with_audio = true;
    auto p = h.default_params(0);
    p.audio_channels = 2;
    p.audio_sample_rate = 48000;
    ASSERT_EQ(h.open(p), EM_OK);
    h.start();
    h.card->advance(10 * kSecond);

    const auto recs = h.card->audio(0);
    ASSERT_GT(recs.size(), 590u);
    uint64_t total = 0, want_total = 0;
    for (size_t k = 0; k < recs.size(); ++k) {
        want_total += (k % 5 == 4) ? 800 : 801;
        EXPECT_NEAR(static_cast<double>(recs[k].stream_time_ns), static_cast<double>(frame_ns(k)), 1000.0)
            << "audio for frame " << k << " must start with its video (within 1 us)";
        ASSERT_EQ(recs[k].samples, h.audio_sent.at(k)) << "samples changed, frame " << k;
        total += recs[k].samples.size() / 2;
    }
    // 801/801/801/801/800 keeps exactly 48 kHz against 59.94 over every 5 frames.
    EXPECT_EQ(total, want_total);
    EXPECT_EQ((recs.size() / 5) * 4004 * 60000 / 1001 % 48000, 0u) << "every 5 frames is exactly 4004 samples";
}

TEST(DeckLink, GenlockLocksFollowsHouseSyncAndReportsLoss) {
    // Reference at +100 ppm against the host clock.
    Harness h;
    h.card->set_reference({true, 100.0});
    auto p = h.default_params(0);
    p.genlock = true;
    ASSERT_EQ(h.open(p), EM_OK);
    EXPECT_EQ(h.status().genlock, EM_GENLOCK_LOCKING);
    h.start();
    h.card->advance(1 * kSecond);
    EXPECT_EQ(h.status().genlock, EM_GENLOCK_LOCKED);
    ASSERT_FALSE(h.status_changes.empty());
    EXPECT_EQ(h.status_changes.back().genlock, EM_GENLOCK_LOCKED);

    h.card->advance(599 * kSecond);
    // The SDI output is the master clock: it plays house sync's rate, not the
    // host's. Ten minutes at +100 ppm is ~3.6 extra frames; no drops either way.
    const double expected = 600.0 * 60000 / 1001 * (1 + 100e-6);
    EXPECT_NEAR(static_cast<double>(h.card->shown(0).size()), expected, 2.0);
    EXPECT_EQ(h.status().frames_dropped, 0u);

    h.card->set_reference({false, 0});
    h.card->advance(100'000'000);
    EXPECT_EQ(h.status().genlock, EM_GENLOCK_NO_REFERENCE);
    EXPECT_EQ(h.status_changes.back().genlock, EM_GENLOCK_NO_REFERENCE) << "loss is reported, not discovered";
}

TEST(DeckLink, DisplayTimesFollowTheCardsCrystal) {
    Harness h;
    h.card->set_crystal_ppm(-50);
    ASSERT_EQ(h.open(h.default_params(0)), EM_OK);
    h.start();
    h.card->advance(600 * kSecond);
    const uint64_t last = h.display_times.rbegin()->first;
    const double host_span = static_cast<double>(h.display_times.rbegin()->second - h.display_times.at(3));
    // A slow card (-50 ppm) shows each frame for longer in host time.
    EXPECT_NEAR(host_span / frame_ns(last - 3), 1.0 / (1 - 50e-6), 2e-6);
    EXPECT_EQ(h.status().frames_dropped, 0u);
}

TEST(DeckLink, ExternalKeyTakesTheConnectorPair) {
    {
        Harness h;
        auto p = h.default_params(0, EM_PIXEL_FORMAT_V210);
        p.key_mode = EM_KEY_MODE_EXTERNAL;
        EXPECT_EQ(h.open(p), EM_ERR_UNSUPPORTED) << "keying needs BGRA (alpha)";
    }
    Harness h;
    h.full_fill = true;
    auto p = h.default_params(0, EM_PIXEL_FORMAT_BGRA8);
    p.key_mode = EM_KEY_MODE_EXTERNAL;
    ASSERT_EQ(h.open(p), EM_OK);
    EXPECT_EQ(h.req.key_row_bytes, 0u) << "no separate key plane; the card keys from alpha";

    // Connector 2 now carries the key.
    em_output_open_params p1 = h.default_params(1);
    em_output_callbacks cb{};
    cb.struct_size = sizeof cb;
    cb.frame_needed = [](void*, uint64_t, int64_t, int64_t) {};
    cb.frame_completed = [](void*, uint64_t, em_frame_result) {};
    em_output_handle* other = nullptr;
    EXPECT_EQ(h.out.open(h.out.provider_ctx, &p1, &cb, &other), EM_ERR_BUSY);

    h.start();
    h.card->advance(100'000'000);
    const auto key = h.card->last_key_plane(0);
    ASSERT_EQ(key.size(), 1920u * 1080u);
    std::vector<uint8_t> frame(h.req.size_bytes);
    const uint64_t shown = h.card->shown(0).back().cookie;
    h.fill(shown, frame.data(), true);
    EXPECT_EQ(key[0], frame[3]);
    EXPECT_EQ(key[1919], frame[1919 * 4 + 3]);

    h.out.stop(h.handle);
    h.out.close(h.handle);
    h.handle = nullptr;
    EXPECT_EQ(h.out.open(h.out.provider_ctx, &p1, &cb, &other), EM_OK) << "closing frees the pair";
    h.out.close(other);
}

TEST(DeckLink, UnplugReportsDeviceLost) {
    Harness h;
    ASSERT_EQ(h.open(h.default_params(0)), EM_OK);
    h.start();
    h.card->advance(kSecond);
    h.card->unplug();
    ASSERT_FALSE(h.status_changes.empty());
    EXPECT_EQ(h.status_changes.back().last_error, EM_ERR_DEVICE_LOST);
    em_video_buffer b{};
    b.struct_size = sizeof b;
    b.format = EM_PIXEL_FORMAT_V210;
    b.width = 1920;
    b.height = 1080;
    b.row_bytes = h.req.row_bytes;
    b.data = h.pool[0].data();
    b.frame_number = 1000;
    EXPECT_EQ(h.out.schedule_video(h.handle, &b), EM_ERR_DEVICE_LOST);
    em_device_info devs[4];
    uint32_t n = 99;
    h.out.enumerate(h.out.provider_ctx, devs, 4, &n);
    EXPECT_EQ(n, 0u) << "a removed card disappears from enumeration";
}

TEST(DeckLink, StopFlushesWhatIsStillScheduled) {
    Harness h;
    ASSERT_EQ(h.open(h.default_params(0)), EM_OK);
    h.start();
    h.card->advance(kSecond);
    const size_t before = h.count(EM_FRAME_FLUSHED);
    ASSERT_EQ(h.out.stop(h.handle), EM_OK);
    EXPECT_EQ(h.count(EM_FRAME_FLUSHED) - before, h.params.preroll_frames);
    h.out.close(h.handle);
    h.handle = nullptr;
}
