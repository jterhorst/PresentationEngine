// SPDX-License-Identifier: MPL-2.0
#include "core/document_index.h"

namespace engine {

PerspectiveSpec to_perspective_spec(const v1::Perspective& p) {
    PerspectiveSpec s;
    s.distance = p.distance();
    s.orthographic = p.orthographic();
    if (p.has_vanishing_point()) {
        s.has_vanishing_point = true;
        s.vanishing_point = {p.vanishing_point().x(), p.vanishing_point().y()};
    }
    return s;
}

DocumentIndex::DocumentIndex(v1::Document document) : document_(std::move(document)) {
    if (document_.canvas().width() > 0 && document_.canvas().height() > 0) {
        canvas_w_ = document_.canvas().width();
        canvas_h_ = document_.canvas().height();
    }
    // Reserve first: ElementRef and the Ref arrays hold pointers into document_,
    // which never moves after this point.
    size_t element_count = 0;
    for (const auto& p : document_.presentations())
        for (const auto& s : p.slides()) element_count += s.elements_size();
    for (const auto& f : document_.fixtures()) element_count += f.elements_size();
    elements.reserve(element_count);

    for (const auto& p : document_.presentations()) {
        PresentationRef pr{&p, static_cast<uint32_t>(slides.size()), static_cast<uint32_t>(p.slides_size())};
        presentations.push_back(pr);
        for (const auto& s : p.slides()) {
            SlideRef sr;
            sr.slide = &s;
            sr.presentation = static_cast<uint32_t>(presentations.size() - 1);
            sr.container = add_container(s.elements(), s.has_perspective() ? &s.perspective() : nullptr);
            slides.push_back(sr);
        }
    }
    for (const auto& f : document_.fixtures()) {
        FixtureRef fr;
        fr.fixture = &f;
        fr.container = add_container(f.elements(), f.has_perspective() ? &f.perspective() : nullptr);
        fixtures.push_back(fr);
    }
    media_resources.assign(document_.media_size(), kNoResource);
}

ContainerRef DocumentIndex::add_container(const google::protobuf::RepeatedPtrField<v1::Element>& els,
                                          const v1::Perspective* own) {
    ContainerRef c;
    c.first_element = static_cast<uint32_t>(elements.size());
    c.element_count = static_cast<uint32_t>(els.size());
    if (own)
        c.camera = to_perspective_spec(*own);
    else if (document_.has_default_perspective())
        c.camera = to_perspective_spec(document_.default_perspective());
    for (const auto& e : els) {
        ElementRef r;
        r.element = &e;
        if (e.has_media())
            if (auto m = find_media(e.media().media_id())) r.media_index = static_cast<int32_t>(*m);
        elements.push_back(r);
    }
    return c;
}

std::optional<uint32_t> DocumentIndex::find_slide(std::string_view presentation_id, std::string_view slide_id) const {
    for (const auto& p : presentations) {
        if (p.presentation->id() != presentation_id) continue;
        for (uint32_t i = 0; i < p.slide_count; ++i)
            if (slides[p.first_slide + i].slide->id() == slide_id) return p.first_slide + i;
    }
    return std::nullopt;
}

std::optional<uint32_t> DocumentIndex::find_fixture(std::string_view fixture_id) const {
    for (uint32_t i = 0; i < fixtures.size(); ++i)
        if (fixtures[i].fixture->id() == fixture_id) return i;
    return std::nullopt;
}

std::optional<uint32_t> DocumentIndex::find_media(std::string_view media_id) const {
    for (int i = 0; i < document_.media_size(); ++i)
        if (document_.media(i).id() == media_id) return static_cast<uint32_t>(i);
    return std::nullopt;
}

}  // namespace engine
