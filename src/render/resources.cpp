// SPDX-License-Identifier: MPL-2.0
#include "render/resources.h"

namespace engine {

namespace {

rhi::TextureId upload(rhi::Device& device, const HalfBitmap& bmp, const char* label, PrepareReport& report) {
    rhi::TextureDesc d;
    d.width = bmp.width;
    d.height = bmp.height;
    d.label = label;
    const rhi::TextureId id = device.create_texture(d);
    device.upload_texture(id, bmp.pixels.data(), bmp.bytes_per_row());
    report.bytes_uploaded += bmp.pixels.size() * sizeof(uint16_t);
    return id;
}

}  // namespace

PrepareReport prepare_resources(DocumentIndex& doc, rhi::Device& device, TextRasterizer& text,
                                ImageDecoder& images, double scale) {
    PrepareReport report;
    const auto& media = doc.document().media();
    for (int i = 0; i < media.size(); ++i) {
        const auto& m = media[i];
        if (m.kind() != v1::MEDIA_KIND_IMAGE) {
            report.warnings.push_back("media " + m.id() + ": only stills in the prototype");
            continue;
        }
        HalfBitmap bmp;
        std::string error;
        if (!images.decode(m.hint_path(), bmp, error)) {
            report.warnings.push_back("media " + m.id() + ": " + error);
            continue;
        }
        doc.media_resources[i] = upload(device, bmp, "media", report);
        ++report.media_textures;
    }
    for (auto& ref : doc.elements) {
        const v1::Element& e = *ref.element;
        if (!e.has_text()) continue;
        HalfBitmap bmp;
        std::string error;
        if (!text.rasterize(e.text(), e.transform().size().x(), e.transform().size().y(), scale, bmp, error)) {
            report.warnings.push_back("text " + e.id() + ": " + error);
            continue;
        }
        ref.resource = upload(device, bmp, "text", report);
        ++report.text_textures;
    }
    return report;
}

void release_resources(DocumentIndex& doc, rhi::Device& device) {
    for (auto& ref : doc.elements)
        if (ref.resource != kNoResource) {
            device.destroy_texture(ref.resource);
            ref.resource = kNoResource;
        }
    for (auto& r : doc.media_resources)
        if (r != kNoResource) {
            device.destroy_texture(r);
            r = kNoResource;
        }
}

}  // namespace engine
