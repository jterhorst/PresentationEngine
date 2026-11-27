// engined: the engine process. Phase 0 prototype modes:
//
//   engined --preview <scene.json>...            test preview window (a screen output)
//   engined --render <scene.json> --at <ms> --out <file.png>
//   engined --goldens <scene.json>... --out-dir <dir>
//   engined --counters <scene.json>...           deterministic per-frame counters as JSON
//
// Phase 1 adds the protocol server (--headless, Unix socket).
//
// SPDX-License-Identifier: MPL-2.0
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "engined/png_writer.h"
#include "engined/scene_runner.h"
#include "rhi_metal/metal_device.h"
#include "text/coretext_layout.h"

namespace engine {
int run_preview(const std::vector<std::string>& scenes);  // preview_window.mm
}

using namespace engine;

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  engined --preview <scene.json>...\n"
                 "  engined --render <scene.json> --at <ms> --out <file.png>\n"
                 "  engined --goldens <scene.json>... --out-dir <dir>\n"
                 "  engined --counters <scene.json>...\n");
    return 2;
}

std::string option(const std::vector<std::string>& args, const char* name, const char* fallback = "") {
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == name) return args[i + 1];
    return fallback;
}

std::vector<std::string> scene_args(const std::vector<std::string>& args) {
    std::vector<std::string> out;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i].rfind("--", 0) == 0) {
            if (args[i] != "--preview" && args[i] != "--goldens" && args[i] != "--counters" && args[i] != "--render")
                ++i;  // skip the option's value
            continue;
        }
        out.push_back(args[i]);
    }
    return out;
}

struct Engine {
    std::unique_ptr<rhi_metal::MetalDevice> device;
    CoreTextRasterizer text;
    ImageIODecoder images;
};

bool make_engine(Engine& e) {
    std::string error;
    e.device = rhi_metal::MetalDevice::create(error);
    if (!e.device) std::fprintf(stderr, "engined: %s\n", error.c_str());
    return e.device != nullptr;
}

std::unique_ptr<SceneRunner> load(Engine& e, const std::string& path) {
    std::string error;
    auto r = SceneRunner::load(path, *e.device, e.text, e.images, error);
    if (!r) std::fprintf(stderr, "engined: %s\n", error.c_str());
    else
        for (const auto& w : r->prepare_report().warnings) std::fprintf(stderr, "engined: warning: %s\n", w.c_str());
    return r;
}

std::string scene_name(const SceneRunner& r, const std::string& path) {
    if (!r.scene().name().empty()) return r.scene().name();
    return std::filesystem::path(path).stem().stem().string();
}

int render_one(const std::vector<std::string>& args) {
    Engine e;
    if (!make_engine(e)) return 1;
    auto r = load(e, option(args, "--render"));
    if (!r) return 1;
    std::vector<uint8_t> bgra;
    const Nanos t = ms_to_ns(std::stoll(option(args, "--at", "0")));
    if (!r->render_bgra(t, bgra)) return 1;
    const auto out = option(args, "--out", "frame.png");
    return write_png_bgra(out, bgra.data(), r->scene().output().width() ? r->scene().output().width() : 1920,
                          r->scene().output().height() ? r->scene().output().height() : 1080)
               ? 0
               : 1;
}

int goldens(const std::vector<std::string>& args) {
    Engine e;
    if (!make_engine(e)) return 1;
    const std::string dir = option(args, "--out-dir", ".");
    std::filesystem::create_directories(dir);
    for (const auto& path : scene_args(args)) {
        auto r = load(e, path);
        if (!r) return 1;
        const auto name = scene_name(*r, path);
        for (uint32_t ms : r->scene().golden_at_ms()) {
            std::vector<uint8_t> bgra;
            if (!r->render_bgra(ms_to_ns(ms), bgra)) return 1;
            const auto file = dir + "/" + name + "_" + std::to_string(ms) + "ms.png";
            write_png_bgra(file, bgra.data(), r->compositor().output().width, r->compositor().output().height);
            std::printf("%s\n", file.c_str());
        }
    }
    return 0;
}

int counters(const std::vector<std::string>& args) {
    Engine e;
    if (!make_engine(e)) return 1;
    std::printf("{\n");
    const auto paths = scene_args(args);
    for (size_t i = 0; i < paths.size(); ++i) {
        auto r = load(e, paths[i]);
        if (!r) return 1;
        uint64_t frames = 0;
        const FrameCounters c = r->measure(frames);
        std::printf("  \"%s\": {\"frames\": %llu, \"allocations\": %llu, \"lock_waits\": %llu, "
                    "\"driver_allocations\": %llu, \"render_passes\": %llu, \"draws\": %llu, \"backdrop_copies\": %llu, "
                    "\"offscreen_textures\": %llu, \"bytes_uploaded\": %llu, \"shaders_compiled\": %llu}%s\n",
                    scene_name(*r, paths[i]).c_str(), (unsigned long long)frames, (unsigned long long)c.allocations,
                    (unsigned long long)c.lock_waits, (unsigned long long)c.driver_allocations,
                    (unsigned long long)c.render_passes,
                    (unsigned long long)c.draws, (unsigned long long)c.backdrop_copies,
                    (unsigned long long)c.offscreen_textures, (unsigned long long)c.bytes_uploaded,
                    (unsigned long long)c.shaders_compiled, i + 1 < paths.size() ? "," : "");
    }
    std::printf("}\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv, argv + argc);
    if (argc < 2) return usage();
    const std::string mode = args[1];
    if (mode == "--preview") return run_preview(scene_args(args));
    if (mode == "--render") return render_one(args);
    if (mode == "--goldens") return goldens(args);
    if (mode == "--counters") return counters(args);
    return usage();
}
