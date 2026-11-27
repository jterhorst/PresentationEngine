// The engine must match tests/reference/projection_cases.json within its tolerance.
// SPDX-License-Identifier: MPL-2.0
#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>

#include "core/projection.h"

using namespace engine;
using google::protobuf::ListValue;
using google::protobuf::Struct;
using google::protobuf::Value;

namespace {

Vec2d vec2(const Value& v) { return {v.list_value().values(0).number_value(), v.list_value().values(1).number_value()}; }

double number(const Struct& s, const char* key, double fallback) {
    auto it = s.fields().find(key);
    return it == s.fields().end() ? fallback : it->second.number_value();
}

TransformSpec to_spec(const Struct& t) {
    TransformSpec s;
    const auto& pos = t.fields().at("position").list_value();
    s.position = {pos.values(0).number_value(), pos.values(1).number_value(), pos.values(2).number_value()};
    s.size = vec2(t.fields().at("size"));
    if (t.fields().count("anchor")) s.anchor = vec2(t.fields().at("anchor"));
    if (t.fields().count("scale")) s.scale = vec2(t.fields().at("scale"));
    s.rotation_deg = number(t, "rotation_deg", 0);
    s.rotation_x_deg = number(t, "rotation_x_deg", 0);
    s.rotation_y_deg = number(t, "rotation_y_deg", 0);
    return s;
}

PerspectiveSpec to_perspective(const Value& v) {
    PerspectiveSpec p;
    if (v.has_null_value()) return p;
    const Struct& s = v.struct_value();
    p.distance = number(s, "distance", 0);
    if (s.fields().count("vanishing_point")) {
        p.has_vanishing_point = true;
        p.vanishing_point = vec2(s.fields().at("vanishing_point"));
    }
    if (s.fields().count("orthographic")) p.orthographic = s.fields().at("orthographic").bool_value();
    return p;
}

}  // namespace

TEST(Projection, MatchesReferenceCases) {
    std::ifstream in(ENGINE_SOURCE_DIR "/tests/reference/projection_cases.json");
    ASSERT_TRUE(in);
    std::stringstream buf;
    buf << in.rdbuf();
    Struct fixture;
    ASSERT_TRUE(google::protobuf::util::JsonStringToMessage(buf.str(), &fixture).ok());

    const auto canvas = vec2(fixture.fields().at("canvas"));
    const double tol = fixture.fields().at("tolerance").number_value();
    const auto& cases = fixture.fields().at("cases").list_value();
    ASSERT_GE(cases.values_size(), 9);

    for (const auto& c : cases.values()) {
        const Struct& cs = c.struct_value();
        SCOPED_TRACE(cs.fields().at("name").string_value());
        const auto camera = resolve_camera(canvas.x, canvas.y, to_perspective(cs.fields().at("perspective")));
        const Projected got = project(to_spec(cs.fields().at("transform").struct_value()), camera);
        const Struct& want = cs.fields().at("expected").struct_value();

        EXPECT_EQ(got.visible, want.fields().at("visible").bool_value());
        EXPECT_NEAR(got.depth, want.fields().at("depth").number_value(), tol);
        if (!got.visible) continue;
        const auto& corners = want.fields().at("corners").list_value();
        for (int i = 0; i < 4; ++i) {
            const Vec2d w = vec2(corners.values(i));
            EXPECT_NEAR(got.corners[i].x, w.x, tol) << "corner " << i;
            EXPECT_NEAR(got.corners[i].y, w.y, tol) << "corner " << i;
        }
    }
}

TEST(Projection, FlatElementsTakeTheFastPath) {
    TransformSpec t;
    t.size = {100, 100};
    EXPECT_TRUE(project(t, resolve_camera(1920, 1080, {})).flat);
    t.rotation_deg = 30;  // rotation around the view axis stays 2D
    EXPECT_TRUE(project(t, resolve_camera(1920, 1080, {})).flat);
    t.rotation_y_deg = 10;
    EXPECT_FALSE(project(t, resolve_camera(1920, 1080, {})).flat);
}
