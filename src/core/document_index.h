// A loaded document flattened for the render thread: slides, fixtures and
// elements in contiguous arrays, ids resolved to indices. Built once when the
// document loads (off the render thread); read-only afterwards.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "core/projection.h"
#include "engine/v1/document.pb.h"

namespace engine {

constexpr uint32_t kNoResource = UINT32_MAX;

struct ElementRef {
    const v1::Element* element = nullptr;
    uint32_t resource = kNoResource;  // GPU texture for text and media, filled in by the preparer
    int32_t media_index = -1;         // index into Document.media for media elements
};

struct ContainerRef {
    uint32_t first_element = 0;
    uint32_t element_count = 0;
    PerspectiveSpec camera;           // container's own, else the document default
};

struct SlideRef {
    const v1::Slide* slide = nullptr;
    uint32_t presentation = 0;
    ContainerRef container;
};

struct FixtureRef {
    const v1::FixtureDef* fixture = nullptr;
    ContainerRef container;
};

struct PresentationRef {
    const v1::Presentation* presentation = nullptr;
    uint32_t first_slide = 0;
    uint32_t slide_count = 0;
};

class DocumentIndex {
public:
    explicit DocumentIndex(v1::Document document);
    DocumentIndex(const DocumentIndex&) = delete;
    DocumentIndex& operator=(const DocumentIndex&) = delete;

    const v1::Document& document() const { return document_; }
    double canvas_width() const { return canvas_w_; }
    double canvas_height() const { return canvas_h_; }

    std::optional<uint32_t> find_slide(std::string_view presentation_id, std::string_view slide_id) const;
    std::optional<uint32_t> find_fixture(std::string_view fixture_id) const;
    std::optional<uint32_t> find_media(std::string_view media_id) const;

    std::vector<PresentationRef> presentations;
    std::vector<SlideRef> slides;            // all presentations, in order
    std::vector<FixtureRef> fixtures;
    std::vector<ElementRef> elements;        // every container's elements, contiguous per container
    std::vector<uint32_t> media_resources;   // per Document.media entry; kNoResource until prepared

private:
    ContainerRef add_container(const google::protobuf::RepeatedPtrField<v1::Element>& elements,
                               const v1::Perspective* own);

    v1::Document document_;
    double canvas_w_ = 1920, canvas_h_ = 1080;
};

PerspectiveSpec to_perspective_spec(const v1::Perspective& p);

}  // namespace engine
