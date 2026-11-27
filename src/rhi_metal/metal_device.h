// Metal backend for the RHI, written against metal-cpp.
//
// Apple silicon only (unified memory): content textures and conversion
// buffers use shared storage, so the CPU writes them directly and an SDI
// module can read a converted frame with no copy.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "rhi/rhi.h"

namespace MTL {
class Device;
class CommandBuffer;
class Texture;
}  // namespace MTL

namespace engine::rhi_metal {

class MetalCommandList;

class MetalDevice final : public rhi::Device {
public:
    // Returns nullptr (and fills `error`) when there's no Metal device or a
    // shader fails to compile.
    static std::unique_ptr<MetalDevice> create(std::string& error);
    ~MetalDevice() override;

    rhi::TextureId create_texture(const rhi::TextureDesc& desc) override;
    void destroy_texture(rhi::TextureId id) override;
    void upload_texture(rhi::TextureId id, const void* data, size_t bytes_per_row) override;
    bool texture_size(rhi::TextureId id, uint32_t& w, uint32_t& h) const override;
    bool read_texture(rhi::TextureId id, void* out, size_t bytes_per_row) override;
    rhi::BufferId create_buffer(size_t bytes, const char* label) override;
    void* buffer_contents(rhi::BufferId id) override;
    rhi::CommandList& begin_frame() override;
    void end_frame(void (*on_complete)(void* ctx), void* ctx) override;
    void wait_idle() override;
    FrameCounters take_counters() override;

    // Platform hooks for the screen output.
    MTL::Device* native_device() const;
    rhi::TextureId wrap_external(MTL::Texture* texture);         // e.g. a CAMetalDrawable's texture
    void set_external(rhi::TextureId id, MTL::Texture* texture);  // swap what an id points at
    MTL::CommandBuffer* current_command_buffer() const;

    struct Impl;

private:
    explicit MetalDevice(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace engine::rhi_metal
