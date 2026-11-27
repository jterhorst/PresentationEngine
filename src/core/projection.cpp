// SPDX-License-Identifier: MPL-2.0
#include "core/projection.h"

#include <cmath>
#include <numbers>

namespace engine {

namespace {
constexpr double kNearFraction = 0.99;  // corners at z >= 0.99 * distance are at or behind the camera
double radians(double deg) { return deg * std::numbers::pi / 180.0; }
}  // namespace

ResolvedCamera resolve_camera(double canvas_w, double canvas_h, const PerspectiveSpec& p) {
    ResolvedCamera c;
    c.distance = p.distance > 0 ? p.distance : canvas_w;
    c.vanishing_point = p.has_vanishing_point ? p.vanishing_point : Vec2d{canvas_w / 2.0, canvas_h / 2.0};
    c.orthographic = p.orthographic;
    return c;
}

std::array<Vec3d, 4> transform_corners(const TransformSpec& t) {
    const double w = t.size.x, h = t.size.y;
    const double ax = t.anchor.x, ay = t.anchor.y;
    const double rz = radians(t.rotation_deg), ry = radians(t.rotation_y_deg), rx = radians(t.rotation_x_deg);
    const Vec3d pivot{t.position.x + ax * w, t.position.y + ay * h, t.position.z};
    const double uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};

    std::array<Vec3d, 4> out;
    for (int i = 0; i < 4; ++i) {
        double px = (uv[i][0] - ax) * w * t.scale.x, py = (uv[i][1] - ay) * h * t.scale.y, pz = 0;
        // 1. Around Z, clockwise on screen (y points down).
        double nx = px * std::cos(rz) - py * std::sin(rz), ny = px * std::sin(rz) + py * std::cos(rz);
        px = nx, py = ny;
        // 2. Around Y: positive moves the right edge away.
        nx = px * std::cos(ry) + pz * std::sin(ry);
        double nz = -px * std::sin(ry) + pz * std::cos(ry);
        px = nx, pz = nz;
        // 3. Around X: positive moves the top edge away.
        ny = py * std::cos(rx) - pz * std::sin(rx);
        nz = py * std::sin(rx) + pz * std::cos(rx);
        py = ny, pz = nz;
        out[i] = {pivot.x + px, pivot.y + py, pivot.z + pz};
    }
    return out;
}

Projected project(const TransformSpec& t, const ResolvedCamera& camera) {
    Projected p;
    const auto world = transform_corners(t);
    p.depth = (world[0].z + world[1].z + world[2].z + world[3].z) / 4.0;
    p.flat = t.position.z == 0 && t.rotation_x_deg == 0 && t.rotation_y_deg == 0;
    if (!camera.orthographic)
        for (const auto& c : world)
            if (c.z >= kNearFraction * camera.distance) return p;  // culled
    const auto& vp = camera.vanishing_point;
    for (int i = 0; i < 4; ++i) {
        const double s = camera.orthographic ? 1.0 : camera.distance / (camera.distance - world[i].z);
        p.corners[i] = {vp.x + (world[i].x - vp.x) * s, vp.y + (world[i].y - vp.y) * s};
        p.w[i] = 1.0 / s;
    }
    p.visible = true;
    return p;
}

}  // namespace engine
