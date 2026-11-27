// The GPU seam: a thin render hardware interface. Just enough for 2D
// compositing with perspective quads and compute conversion. Metal is the
// first backend (src/rhi_metal); Vulkan or D3D12 later (architecture.md,
// "Platform seams").
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/counters.h"
#include "core/draw_params.h"

namespace engine::rhi {

using TextureId = uint32_t;
using BufferId = uint32_t;
constexpr TextureId kNoTexture = UINT32_MAX;

enum class PixelFormat : uint8_t {
    RGBA16Float,  // composite targets and prepared content: linear, premultiplied
    BGRA8Unorm,   // screens, sRGB-encoded by the convert pass
};

struct TextureDesc {
    uint32_t width = 0, height = 0;
    PixelFormat format = PixelFormat::RGBA16Float;
    bool render_target = false;
    bool shader_write = false;
    bool cpu_readable = false;  // golden tests read pixels back
    const char* label = "";
};

enum class Blend : uint8_t {
    Replace,             // shader wrote the full composite (backdrop path)
    PremultipliedOver,   // NORMAL
    Add,                 // ADD
};

enum class ConvertKind : uint8_t { UYVY, V210 };

// Records GPU work for one frame. Not thread-safe; one per output per frame.
class CommandList {
public:
    virtual ~CommandList() = default;

    // `clear` = nullptr keeps the target's contents.
    virtual void begin_render_pass(TextureId target, const float4* clear) = 0;
    virtual void draw_quad(const DrawParams& params, TextureId t0, TextureId t1, Blend blend) = 0;
    virtual void end_render_pass() = 0;

    virtual void copy_texture(TextureId src, TextureId dst) = 0;  // same size and format

    // The output conversion pass (shaders/core/convert.slang).
    virtual void convert_to_bgra8(TextureId src, TextureId dst) = 0;
    virtual void convert_to_buffer(ConvertKind kind, TextureId src, BufferId dst, uint32_t row_words) = 0;
};

class Device {
public:
    virtual ~Device() = default;

    virtual TextureId create_texture(const TextureDesc& desc) = 0;
    virtual void destroy_texture(TextureId id) = 0;
    // Uploads a whole texture from tightly packed rows.
    virtual void upload_texture(TextureId id, const void* data, size_t bytes_per_row) = 0;
    virtual bool texture_size(TextureId id, uint32_t& w, uint32_t& h) const = 0;
    // Reads back a cpu_readable texture after the GPU finished with it.
    virtual bool read_texture(TextureId id, void* out, size_t bytes_per_row) = 0;

    // GPU-shared buffers: on Apple silicon the same memory DeckLink reads (zero copy).
    virtual BufferId create_buffer(size_t bytes, const char* label) = 0;
    virtual void* buffer_contents(BufferId id) = 0;

    virtual CommandList& begin_frame() = 0;
    // Commits the frame. `on_complete` runs on a GPU completion thread.
    virtual void end_frame(void (*on_complete)(void* ctx), void* ctx) = 0;
    virtual void wait_idle() = 0;

    // Counters accumulated since the last call, then reset.
    virtual FrameCounters take_counters() = 0;
};

}  // namespace engine::rhi
