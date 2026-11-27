// GPU tests (Apple silicon): shader conversions against the CPU reference,
// golden images of every reference scene, and the per-frame counter gate.
//
//   ENGINE_UPDATE_GOLDENS=1   rewrite tests/golden/*.png from this machine
//   ENGINE_UPDATE_BASELINE=1  rewrite perf/baseline.json (lower is automatic; higher needs review)
//   ENGINE_SKIP_GOLDENS=1     counters only (hosted CI: VM GPUs aren't golden-stable)
//
// SPDX-License-Identifier: MPL-2.0
#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>

#include "engined/png_writer.h"
#include "engined/scene_runner.h"
#include "render/convert_reference.h"
#include "rhi_metal/metal_device.h"
#include "text/coretext_layout.h"

using namespace engine;
namespace fs = std::filesystem;

namespace {

const fs::path kRoot = ENGINE_SOURCE_DIR;

bool env(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && std::string(v) != "0";
}

struct Gpu {
    std::unique_ptr<rhi_metal::MetalDevice> device;
    CoreTextRasterizer text;
    ImageIODecoder images;
};

Gpu& gpu() {
    static Gpu g = [] {
        Gpu x;
        std::string error;
        x.device = rhi_metal::MetalDevice::create(error);
        if (!x.device) ADD_FAILURE() << error;
        return x;
    }();
    return g;
}

std::vector<fs::path> scenes() {
    std::vector<fs::path> out;
    for (const auto& e : fs::directory_iterator(kRoot / "scenes"))
        if (e.path().string().ends_with(".scene.json")) out.push_back(e.path());
    std::sort(out.begin(), out.end());
    return out;
}

std::string stem(const fs::path& p) { return p.stem().stem().string(); }

// Half-float bits for the conversion tests' input texture.
uint16_t to_half(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000;
    int32_t exp = static_cast<int32_t>((x >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = x & 0x7FFFFF;
    if (exp <= 0) return static_cast<uint16_t>(sign);
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7C00);
    // round to nearest
    mant += 0x1000;
    if (mant & 0x800000) {
        mant = 0;
        ++exp;
    }
    return static_cast<uint16_t>(sign | (exp << 10) | (mant >> 13));
}

float from_half(uint16_t h) {
    const uint32_t sign = (h & 0x8000) << 16, exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
    uint32_t x = exp == 0 ? sign : sign | ((exp - 15 + 127) << 23) | (mant << 13);
    float f;
    std::memcpy(&f, &x, 4);
    return f;
}

}  // namespace

// ---- Shader layer: convert.slang vs convert_reference.h -------------------------

TEST(Shaders, UyvyAndV210MatchTheCpuReference) {
    auto& dev = *gpu().device;
    constexpr uint32_t W = 48, H = 4;
    std::vector<convert_ref::Pixel> px;
    std::vector<uint16_t> half;
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            const float a = (x % 7 == 0) ? 0.5f : 1.0f;
            convert_ref::Pixel p{x / float(W) * a, y / float(H) * a, ((x * 13 + y * 7) % 17) / 16.0f * a, a};
            // Round through half precision so both sides see identical input.
            for (float* c : {&p.r, &p.g, &p.b, &p.a}) {
                *c = from_half(to_half(*c));
                half.push_back(to_half(*c));
            }
            px.push_back(p);
        }
    rhi::TextureDesc d;
    d.width = W;
    d.height = H;
    const auto src = dev.create_texture(d);
    dev.upload_texture(src, half.data(), W * 8);

    const uint32_t uyvy_words = convert_ref::uyvy_row_words(W), v210_words = convert_ref::v210_row_words(W);
    const auto uyvy = dev.create_buffer(uyvy_words * H * 4, "uyvy");
    const auto v210 = dev.create_buffer(v210_words * H * 4, "v210");
    auto& cmd = dev.begin_frame();
    cmd.convert_to_buffer(rhi::ConvertKind::UYVY, src, uyvy, uyvy_words);
    cmd.convert_to_buffer(rhi::ConvertKind::V210, src, v210, v210_words);
    dev.end_frame(nullptr, nullptr);
    dev.wait_idle();

    // Within one code value: GPU pow() and rounding can differ in the last bit.
    auto compare = [](const std::vector<uint32_t>& want, const uint32_t* got, int bits, const char* what) {
        const uint32_t mask = (1u << bits) - 1;
        int worst = 0;
        for (size_t i = 0; i < want.size(); ++i)
            for (int s = 0; s < 32 / bits * bits; s += bits)
                worst = std::max(worst, std::abs(int((want[i] >> s) & mask) - int((got[i] >> s) & mask)));
        EXPECT_LE(worst, 1) << what;
    };
    compare(convert_ref::to_uyvy(px, W, H, uyvy_words), static_cast<uint32_t*>(dev.buffer_contents(uyvy)), 8, "UYVY");
    compare(convert_ref::to_v210(px, W, H, v210_words), static_cast<uint32_t*>(dev.buffer_contents(v210)), 10, "v210");
    dev.destroy_texture(src);
}

// ---- Golden images ---------------------------------------------------------------

TEST(Golden, ReferenceScenesMatchApprovedImages) {
    if (env("ENGINE_SKIP_GOLDENS")) GTEST_SKIP() << "ENGINE_SKIP_GOLDENS set";
    const bool update = env("ENGINE_UPDATE_GOLDENS");
    const fs::path diff_dir = fs::temp_directory_path() / "engine-golden-diff";
    for (const auto& path : scenes()) {
        SCOPED_TRACE(path.filename().string());
        std::string error;
        auto r = SceneRunner::load(path.string(), *gpu().device, gpu().text, gpu().images, error);
        ASSERT_TRUE(r) << error;
        EXPECT_TRUE(r->prepare_report().warnings.empty()) << r->prepare_report().warnings.front();
        const uint32_t w = r->compositor().output().width, h = r->compositor().output().height;
        for (uint32_t ms : r->scene().golden_at_ms()) {
            std::vector<uint8_t> got;
            ASSERT_TRUE(r->render_bgra(ms_to_ns(ms), got));
            const fs::path golden = kRoot / "tests/golden" / (stem(path) + "_" + std::to_string(ms) + "ms.png");
            if (update) {
                ASSERT_TRUE(write_png_bgra(golden.string(), got.data(), w, h));
                continue;
            }
            std::vector<uint8_t> want;
            uint32_t gw = 0, gh = 0;
            ASSERT_TRUE(read_png_bgra(golden.string(), want, gw, gh)) << "missing " << golden << "; run with ENGINE_UPDATE_GOLDENS=1";
            ASSERT_EQ(gw, w);
            ASSERT_EQ(gh, h);
            // Perceptual-ish tolerance: text rasterization and GPU rounding move
            // edges by a level or two; real regressions move many pixels a lot.
            size_t off = 0;
            for (size_t i = 0; i < got.size(); i += 4)
                for (int c = 0; c < 3; ++c)
                    if (std::abs(int(got[i + c]) - int(want[i + c])) > 8) {
                        ++off;
                        break;
                    }
            const double fraction = double(off) / (w * h);
            if (fraction > 0.002) {
                fs::create_directories(diff_dir);
                const auto out = diff_dir / golden.filename();
                write_png_bgra(out.string(), got.data(), w, h);
                ADD_FAILURE() << golden.filename() << ": " << fraction * 100 << "% of pixels differ; got " << out;
            }
        }
    }
}

// ---- Counter gate ----------------------------------------------------------------

TEST(Counters, NoSceneExceedsTheBaseline) {
    const fs::path baseline_path = kRoot / "perf/baseline.json";
    google::protobuf::Struct baseline;
    {
        std::ifstream in(baseline_path);
        std::stringstream buf;
        buf << in.rdbuf();
        if (in) ASSERT_TRUE(google::protobuf::util::JsonStringToMessage(buf.str(), &baseline).ok());
    }
    const bool update = env("ENGINE_UPDATE_BASELINE");
    google::protobuf::Struct fresh;

    for (const auto& path : scenes()) {
        SCOPED_TRACE(path.filename().string());
        std::string error;
        auto r = SceneRunner::load(path.string(), *gpu().device, gpu().text, gpu().images, error);
        ASSERT_TRUE(r) << error;
        uint64_t frames = 0;
        const FrameCounters c = r->measure(frames);
        EXPECT_EQ(c.allocations, 0u) << "render-thread allocations in steady state";
        EXPECT_EQ(c.lock_waits, 0u) << "render-thread lock waits in steady state";

        const std::map<std::string, uint64_t> got = {
            {"frames", frames},
            {"allocations", c.allocations},
            {"lock_waits", c.lock_waits},
            {"driver_allocations", c.driver_allocations},
            {"render_passes", c.render_passes},
            {"draws", c.draws},
            {"backdrop_copies", c.backdrop_copies},
            {"offscreen_textures", c.offscreen_textures},
            {"bytes_uploaded", c.bytes_uploaded},
            {"shaders_compiled", c.shaders_compiled},
        };
        auto& entry = *(*fresh.mutable_fields())[stem(path)].mutable_struct_value()->mutable_fields();
        for (const auto& [k, v] : got) entry[k].set_number_value(static_cast<double>(v));

        auto it = baseline.fields().find(stem(path));
        if (it == baseline.fields().end()) {
            if (!update) ADD_FAILURE() << "no baseline for " << stem(path) << "; run with ENGINE_UPDATE_BASELINE=1";
            continue;
        }
        const auto& base = it->second.struct_value().fields();
        for (const auto& [k, v] : got) {
            // Driver allocations vary run to run (Metal's own bookkeeping), so
            // they're recorded for visibility but never gate.
            if (k == "frames" || k == "driver_allocations") continue;
            const auto b = base.find(k);
            const uint64_t limit = b == base.end() ? 0 : static_cast<uint64_t>(b->second.number_value());
            if (!update) EXPECT_LE(v, limit) << k << " regressed in " << stem(path) << " (baseline " << limit << ")";
            if (v < limit) std::printf("note: %s.%s improved %llu -> %llu; ratchet with ENGINE_UPDATE_BASELINE=1\n",
                                       stem(path).c_str(), k.c_str(), (unsigned long long)limit, (unsigned long long)v);
        }
    }
    if (update) {
        // Written by hand so keys stay sorted and diffs stay readable.
        std::map<std::string, const google::protobuf::Struct*> by_scene;
        for (const auto& [name, v] : fresh.fields()) by_scene[name] = &v.struct_value();
        std::ofstream out(baseline_path);
        out << "{\n";
        size_t i = 0;
        for (const auto& [name, st] : by_scene) {
            std::map<std::string, double> fields;
            for (const auto& [k, v] : st->fields()) fields[k] = v.number_value();
            out << "  \"" << name << "\": {";
            size_t j = 0;
            for (const auto& [k, v] : fields)
                out << (j++ ? ", " : "") << "\"" << k << "\": " << static_cast<uint64_t>(v);
            out << "}" << (++i < by_scene.size() ? "," : "") << "\n";
        }
        out << "}\n";
    }
}
