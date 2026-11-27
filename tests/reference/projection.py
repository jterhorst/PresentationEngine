"""Reference implementation of element 3D projection.

This file IS the spec for how a Transform plus a Perspective maps to screen
corners. The engine (C++), the native editor (Swift, via the shared library)
and the web editor (WASM) must match it within 1e-3 canvas units on the
cases in projection_cases.json. Regenerate that file only on purpose:

    python3 tests/reference/projection.py --write
    python3 tests/reference/projection.py          # self-check only

Coordinates: canvas units, x right, y down, z toward the viewer.
"""
import json
import math
import pathlib
import sys

NEAR_FRACTION = 0.99  # corners at z >= 0.99 * distance are at/behind the camera


def resolve_perspective(canvas_w, canvas_h, perspective=None):
    p = perspective or {}
    distance = p.get("distance") or float(canvas_w)
    vp = p.get("vanishing_point") or (canvas_w / 2.0, canvas_h / 2.0)
    return distance, vp, bool(p.get("orthographic", False))


def transform_corners(t):
    """World-space (X, Y, Z) of the box corners: top-left, top-right, bottom-right, bottom-left."""
    x, y, z = t["position"]
    w, h = t["size"]
    ax, ay = t.get("anchor", (0.5, 0.5))
    sx, sy = t.get("scale", (1.0, 1.0))
    rz = math.radians(t.get("rotation_deg", 0.0))
    ry = math.radians(t.get("rotation_y_deg", 0.0))
    rx = math.radians(t.get("rotation_x_deg", 0.0))
    pivot = (x + ax * w, y + ay * h, z)

    out = []
    for u, v in ((0, 0), (1, 0), (1, 1), (0, 1)):
        px, py, pz = (u - ax) * w * sx, (v - ay) * h * sy, 0.0
        # 1. Rotate around Z (clockwise on screen, since y points down).
        px, py = px * math.cos(rz) - py * math.sin(rz), px * math.sin(rz) + py * math.cos(rz)
        # 2. Rotate around Y: positive moves the right edge (x > 0) away (z < 0).
        px, pz = px * math.cos(ry) + pz * math.sin(ry), -px * math.sin(ry) + pz * math.cos(ry)
        # 3. Rotate around X: positive moves the top edge (y < 0) away (z < 0).
        py, pz = py * math.cos(rx) - pz * math.sin(rx), py * math.sin(rx) + pz * math.cos(rx)
        out.append((pivot[0] + px, pivot[1] + py, pivot[2] + pz))
    return out


def project(t, canvas_w, canvas_h, perspective=None):
    """Returns {"corners": [[sx, sy] x4], "depth": center Z, "visible": bool}."""
    distance, vp, ortho = resolve_perspective(canvas_w, canvas_h, perspective)
    world = transform_corners(t)
    depth = sum(c[2] for c in world) / 4.0
    if not ortho and any(c[2] >= NEAR_FRACTION * distance for c in world):
        return {"corners": None, "depth": depth, "visible": False}
    corners = []
    for X, Y, Z in world:
        s = 1.0 if ortho else distance / (distance - Z)
        corners.append([vp[0] + (X - vp[0]) * s, vp[1] + (Y - vp[1]) * s])
    return {"corners": corners, "depth": depth, "visible": True}


def draw_order(elements, canvas_w, canvas_h, perspective=None):
    """Indices back to front: ascending center depth, ties keep list order."""
    keyed = [(project(e, canvas_w, canvas_h, perspective)["depth"], i) for i, e in enumerate(elements)]
    return [i for _, i in sorted(keyed)]


CANVAS = (1920, 1080)
BOX = {"position": (760, 340, 0), "size": (400, 400)}

CASES = [
    ("flat at z=0 is unchanged", dict(BOX), None),
    ("z=+960 at default distance doubles in size", {**BOX, "position": (760, 340, 960)}, None),
    ("z=-1920 halves in size", {**BOX, "position": (760, 340, -1920)}, None),
    ("rotation_y 45: right edge recedes", {**BOX, "rotation_y_deg": 45}, None),
    ("rotation_x 30: top edge recedes", {**BOX, "rotation_x_deg": 30}, None),
    ("rotation 90 clockwise", {**BOX, "size": (400, 200), "rotation_deg": 90}, None),
    ("off-center vanishing point", {**BOX, "position": (100, 100, -500)},
     {"distance": 1500, "vanishing_point": (0, 0)}),
    ("orthographic ignores depth scale", {**BOX, "position": (760, 340, 900)}, {"orthographic": True}),
    ("behind the camera is culled", {**BOX, "position": (760, 340, 2000)}, None),
]


def self_check(results):
    by = {name: r for name, r in results}
    tl, tr, br, bl = by["flat at z=0 is unchanged"]["corners"]
    assert all(abs(a - b) < 1e-9 for a, b in zip(tl + br, [760, 340, 1160, 740])), "z=0 must be identity"
    c = by["z=+960 at default distance doubles in size"]["corners"]
    assert abs((c[1][0] - c[0][0]) - 800) < 1e-6, "positive z must enlarge (toward viewer)"
    c = by["z=-1920 halves in size"]["corners"]
    assert abs((c[1][0] - c[0][0]) - 200) < 1e-6, "negative z must shrink"
    c = by["rotation_y 45: right edge recedes"]["corners"]
    assert (c[2][1] - c[1][1]) < (c[3][1] - c[0][1]), "right edge must be shorter than left"
    c = by["rotation_x 30: top edge recedes"]["corners"]
    assert (c[1][0] - c[0][0]) < (c[2][0] - c[3][0]), "top edge must be shorter than bottom"
    c = by["rotation 90 clockwise"]["corners"]
    assert c[0][0] > c[2][0] and c[0][1] < c[2][1], "clockwise: top-left moves to top-right"
    c = by["orthographic ignores depth scale"]["corners"]
    assert abs((c[1][0] - c[0][0]) - 400) < 1e-9
    assert not by["behind the camera is culled"]["visible"]
    order = draw_order([{**BOX, "position": (0, 0, 50)}, {**BOX, "position": (0, 0, -50)},
                        {**BOX, "position": (0, 0, 50)}], *CANVAS)
    assert order == [1, 0, 2], f"back to front with stable ties, got {order}"


def main():
    results = [(name, project(t, *CANVAS, p)) for name, t, p in CASES]
    self_check(results)
    fixture = {
        "canvas": CANVAS,
        "tolerance": 1e-3,
        "cases": [
            {"name": name, "transform": t, "perspective": p, "expected": r}
            for (name, t, p), (_, r) in zip(CASES, results)
        ],
    }
    path = pathlib.Path(__file__).with_name("projection_cases.json")
    if "--write" in sys.argv:
        path.write_text(json.dumps(fixture, indent=2) + "\n")
        print(f"wrote {path.name}")
    elif path.exists():
        stored = json.loads(path.read_text())
        assert json.loads(json.dumps(fixture)) == stored, "projection_cases.json is out of date"
    print("PASS projection reference self-check" + (" and fixture" if path.exists() else ""))


if __name__ == "__main__":
    main()
