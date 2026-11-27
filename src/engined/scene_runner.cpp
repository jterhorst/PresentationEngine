// SPDX-License-Identifier: MPL-2.0
#include "engined/scene_runner.h"

#include <algorithm>

namespace engine {

std::unique_ptr<SceneRunner> SceneRunner::load(const std::string& path, rhi::Device& device, TextRasterizer& text,
                                               ImageDecoder& images, std::string& error) {
    auto scene = load_reference_scene(path, error);
    if (!scene) return nullptr;
    std::unique_ptr<SceneRunner> r(new SceneRunner(device));
    r->scene_ = std::move(*scene);
    const auto& o = r->scene_.output();
    r->out_.width = o.width() ? o.width() : 1920;
    r->out_.height = o.height() ? o.height() : 1080;
    r->rate_ = o.has_rate() && o.rate().num() > 0 && o.rate().den() > 0 ? FrameRate{o.rate().num(), o.rate().den()}
                                                                         : kRate5994;
    r->doc_ = std::make_unique<DocumentIndex>(r->scene_.document());
    const double scale = r->out_.height / r->doc_->canvas_height();
    r->report_ = prepare_resources(*r->doc_, device, text, images, scale);
    r->compositor_ = std::make_unique<Compositor>(device, r->out_);
    r->sizes_ = std::make_unique<DeviceResourceSizes>(device);
    r->list_.passes.reserve(8);
    r->list_.draws.reserve(256);
    r->restart();
    device.take_counters();  // preparation isn't frame work
    return r;
}

SceneRunner::~SceneRunner() {
    device_.wait_idle();
    if (doc_) release_resources(*doc_, device_);
    if (readback_ != rhi::kNoTexture) device_.destroy_texture(readback_);
}

void SceneRunner::restart() {
    LiveCommand c;
    while (queue_.pop(c)) {
    }
    state_ = LiveState{};
    LiveCommand load;
    load.kind = LiveCommand::Kind::LoadDocument;
    load.document = doc_.get();
    state_.apply(load, 0);
    next_step_ = 0;
}

Nanos SceneRunner::edge_at_or_after(Nanos t) const {
    uint64_t f = rate_.frame_at(t);
    if (rate_.edge(f) < t) ++f;
    return rate_.edge(f);
}

void SceneRunner::advance_script(Nanos t) {
    while (next_step_ < scene_.script_size()) {
        const auto& step = scene_.script(next_step_);
        const Nanos edge = edge_at_or_after(ms_to_ns(step.at_ms()));
        if (edge > t) break;
        std::string error;
        if (auto cmd = to_live_command(step.command(), *doc_, error)) queue_.push(*cmd);
        state_.apply_pending(queue_, edge);
        ++next_step_;
    }
}

bool SceneRunner::send(const v1::Command& command, std::string& error) {
    auto cmd = to_live_command(command, *doc_, error);
    if (!cmd) return false;
    if (!queue_.push(*cmd)) {
        error = "busy";
        return false;
    }
    return true;
}

bool SceneRunner::step_slide(int delta) {
    const auto n = static_cast<int64_t>(doc_->slides.size());
    if (n == 0) return false;
    int64_t i = state_.slide == kNone ? (delta > 0 ? -1 : n) : static_cast<int64_t>(state_.slide);
    i = std::clamp<int64_t>(i + delta, 0, n - 1);
    LiveCommand take;
    take.kind = LiveCommand::Kind::TakeSlide;
    take.index = static_cast<uint32_t>(i);
    return queue_.push(take);
}

void SceneRunner::render(Nanos t, rhi::CommandList& cmd, FrameCounters& counters) {
    AllocationScope allocs;
    state_.apply_pending(queue_, t);
    resolve(state_, t, out_, *sizes_, list_);
    compositor_->encode(list_, cmd);
    counters.allocations += allocs.count();
    counters.driver_allocations += allocs.driver_count();
    counters.offscreen_textures += compositor_->take_offscreen_count();
}

bool SceneRunner::render_bgra(Nanos t, std::vector<uint8_t>& bgra, FrameCounters* counters) {
    if (readback_ == rhi::kNoTexture) {
        rhi::TextureDesc d;
        d.width = out_.width;
        d.height = out_.height;
        d.format = rhi::PixelFormat::BGRA8Unorm;
        d.shader_write = true;
        d.cpu_readable = true;
        d.label = "readback";
        readback_ = device_.create_texture(d);
    }
    advance_script(t);
    FrameCounters c;
    rhi::CommandList& cmd = device_.begin_frame();
    render(t, cmd, c);
    cmd.convert_to_bgra8(compositor_->composite(), readback_);
    device_.end_frame(nullptr, nullptr);
    device_.wait_idle();
    c += device_.take_counters();
    if (counters) *counters = c;
    bgra.resize(static_cast<size_t>(out_.width) * out_.height * 4);
    return device_.read_texture(readback_, bgra.data(), static_cast<size_t>(out_.width) * 4);
}

FrameCounters SceneRunner::measure(uint64_t& frames) {
    FrameCounters total;
    frames = 0;
    for (int pass = 0; pass < 2; ++pass) {
        restart();
        device_.take_counters();
        for (uint64_t f = 0;; ++f) {
            const Nanos t = rate_.edge(f);
            if (t >= duration()) break;
            advance_script(t);
            FrameCounters c;
            rhi::CommandList& cmd = device_.begin_frame();
            render(t, cmd, c);
            device_.end_frame(nullptr, nullptr);
            c += device_.take_counters();
            if (pass == 1) {
                total += c;
                ++frames;
            }
        }
        device_.wait_idle();
    }
    return total;
}

}  // namespace engine
