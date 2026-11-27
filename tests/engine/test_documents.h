// Small documents built in code for the CPU tests.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>

#include "core/document_index.h"

namespace test_docs {

inline engine::v1::Element rect(const char* id, float x, float y, float z, float w = 200, float h = 100) {
    engine::v1::Element e;
    e.set_id(id);
    auto* t = e.mutable_transform();
    t->mutable_position()->set_x(x);
    t->mutable_position()->set_y(y);
    t->mutable_position()->set_z(z);
    t->mutable_size()->set_x(w);
    t->mutable_size()->set_y(h);
    auto* s = e.mutable_shape();
    s->set_kind(engine::v1::SHAPE_KIND_RECTANGLE);
    s->mutable_fill()->set_r(1);
    s->mutable_fill()->set_a(1);
    return e;
}

// Presentation "p" with slides s0..s2. s2 shows fixture "f" and sets a blue background.
inline std::unique_ptr<engine::DocumentIndex> three_slides(uint32_t dissolve_ms = 0) {
    engine::v1::Document d;
    d.set_id("doc");
    d.mutable_canvas()->set_width(1920);
    d.mutable_canvas()->set_height(1080);
    auto* p = d.add_presentations();
    p->set_id("p");
    if (dissolve_ms) {
        p->mutable_default_transition()->set_package_id("core");
        p->mutable_default_transition()->set_transition_id("dissolve");
        p->mutable_default_transition()->set_duration_ms(dissolve_ms);
    }
    for (int i = 0; i < 3; ++i) {
        auto* s = p->add_slides();
        s->set_id("s" + std::to_string(i));
        *s->add_elements() = rect("a", 100, 100, 0);
        *s->add_elements() = rect("b", 300, 100, 50);
        *s->add_elements() = rect("c", 500, 100, -50);
    }
    auto* s2 = p->mutable_slides(2);
    s2->add_actions()->set_show_fixture_id("f");
    s2->mutable_background()->mutable_color()->set_b(1);
    s2->mutable_background()->mutable_color()->set_a(1);
    auto* f = d.add_fixtures();
    f->set_id("f");
    f->set_z(150);
    *f->add_elements() = rect("lower-third", 0, 900, 0, 1920, 180);
    return std::make_unique<engine::DocumentIndex>(std::move(d));
}

}  // namespace test_docs
