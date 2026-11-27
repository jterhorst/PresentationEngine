// Element 3D projection. tests/reference/projection.py is the spec; this must
// match tests/reference/projection_cases.json within 1e-3 canvas units.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>

namespace engine {

struct Vec2d { double x = 0, y = 0; };
struct Vec3d { double x = 0, y = 0, z = 0; };

struct TransformSpec {
    Vec3d position;            // top-left of the box; z toward the viewer
    Vec2d size;
    Vec2d anchor{0.5, 0.5};
    Vec2d scale{1, 1};
    double rotation_deg = 0;   // around the view axis, clockwise on screen
    double rotation_y_deg = 0; // positive: right edge away
    double rotation_x_deg = 0; // positive: top edge away
};

struct PerspectiveSpec {
    double distance = 0;       // 0 = canvas width
    bool has_vanishing_point = false;
    Vec2d vanishing_point;     // unset = canvas center
    bool orthographic = false;
};

struct ResolvedCamera {
    double distance;
    Vec2d vanishing_point;
    bool orthographic;
};

ResolvedCamera resolve_camera(double canvas_w, double canvas_h, const PerspectiveSpec& p);

struct Projected {
    std::array<Vec2d, 4> corners;  // TL, TR, BR, BL in canvas units
    std::array<double, 4> w;       // perspective divisor per corner: (d - Z) / d, 1 when orthographic
    double depth = 0;              // center Z
    bool visible = false;
    bool flat = false;             // z = 0 and no X/Y rotation: the 2D fast path
};

// World-space corners: TL, TR, BR, BL.
std::array<Vec3d, 4> transform_corners(const TransformSpec& t);

Projected project(const TransformSpec& t, const ResolvedCamera& camera);

}  // namespace engine
