"""Writes the reference scenes, scenes/*.scene.json (checked in).

Each scene is a tests/scenes/reference_scene.proto ReferenceScene in protobuf's
JSON mapping: a normal engine.v1.Document, an output, and a timed command
script. They feed the golden images, perf/baseline.json and `engined --preview`.
This script is the readable source; RTF is bytes, so the JSON holds it base64.

    python3 scenes/make_scenes.py
"""
import base64
import json
import math
import pathlib

HERE = pathlib.Path(__file__).resolve().parent
CANVAS = {"width": 1920, "height": 1080}
OUTPUT = {"width": 1920, "height": 1080, "rate": {"num": 60000, "den": 1001}}


# ---- helpers ---------------------------------------------------------------

def color(r, g, b, a=1.0):
    return {"r": r, "g": g, "b": b, "a": a}


def hexc(h, a=1.0):
    h = h.lstrip("#")
    return color(*(int(h[i:i + 2], 16) / 255 for i in (0, 2, 4)), a)


def transform(x, y, w, h, z=0, **rot):
    t = {"position": {"x": x, "y": y, "z": z}, "size": {"x": w, "y": h}}
    t.update(rot)
    return t


def shape(eid, t, kind="SHAPE_KIND_RECTANGLE", fill=None, stroke=None, radius=0, shadow=None, **extra):
    s = {"kind": kind}
    if fill: s["fill"] = fill
    if stroke: s["stroke"] = stroke
    if radius: s["cornerRadius"] = radius
    if shadow: s["shadows"] = [shadow]
    return {"id": eid, "transform": t, "shape": s, **extra}


def rtf_escape(s):
    out = []
    for ch in s:
        if ch in "\\{}": out.append("\\" + ch)
        elif ch == "\n": out.append("\\line ")
        elif ord(ch) > 127: out.append(f"\\u{ord(ch) if ord(ch) < 32768 else ord(ch) - 65536}?")
        else: out.append(ch)
    return "".join(out)


def rtf(paragraphs, fonts, colors):
    """paragraphs: [(para_controls, [(char_controls, text), ...]), ...]"""
    ft = "".join(f"\\f{i}\\fnil {name};" for i, name in enumerate(fonts))
    ct = ";" + "".join(f"\\red{int(c[0]*255)}\\green{int(c[1]*255)}\\blue{int(c[2]*255)};" for c in colors)
    body = []
    for pc, runs in paragraphs:
        body.append("\\pard" + pc + " " + "".join("{" + cc + " " + rtf_escape(t) + "}" for cc, t in runs))
    doc = "{\\rtf1\\ansi\\ansicpg1252\\cocoartf2822{\\fonttbl" + ft + "}{\\colortbl" + ct + "}" + "\\par\n".join(body) + "}"
    return base64.b64encode(doc.encode("ascii")).decode()


def text_el(eid, t, rtf_b64=None, runs=None, align="TEXT_ALIGN_CENTER", valign=None, fit=None, **extra):
    te = {}
    if rtf_b64: te["rtf"] = rtf_b64
    if runs: te["runs"] = runs
    te["paragraph"] = {"align": align}
    if valign: te["verticalAlign"] = valign
    if fit: te["fit"] = fit
    te.update(extra)
    return {"id": eid, "transform": t, "text": te}


def run(text, font, size, fill=(1, 1, 1, 1), **st):
    return {"text": text, "style": {"fontPostscriptName": font, "sizePt": size, "fill": color(*fill), **st}}


def ellipse_path(w, h, x=0, y=0):
    """Four cubic Béziers in box coordinates, bounding box at (x, y)."""
    k = 0.5522847498
    cx, cy, rx, ry = x + w / 2, y + h / 2, w / 2, h / 2
    pt = lambda x, y: {"x": x, "y": y}
    return {"commands": [
        {"moveTo": pt(cx + rx, cy)},
        {"cubicTo": {"control1": pt(cx + rx, cy + k * ry), "control2": pt(cx + k * rx, cy + ry), "to": pt(cx, cy + ry)}},
        {"cubicTo": {"control1": pt(cx - k * rx, cy + ry), "control2": pt(cx - rx, cy + k * ry), "to": pt(cx - rx, cy)}},
        {"cubicTo": {"control1": pt(cx - rx, cy - k * ry), "control2": pt(cx - k * rx, cy - ry), "to": pt(cx, cy - ry)}},
        {"cubicTo": {"control1": pt(cx + k * rx, cy - ry), "control2": pt(cx + rx, cy - k * ry), "to": pt(cx + rx, cy)}},
        {"close": {}},
    ]}


def take(at_ms, pres, slide):
    return {"atMs": at_ms, "command": {"takeSlide": {"presentationId": pres, "slideId": slide}}}


def scene(name, description, document, script, goldens, duration_ms):
    document.setdefault("canvas", CANVAS)
    document.setdefault("modelVersion", 1)
    return {"name": name, "description": description, "document": document, "output": OUTPUT,
            "script": script, "goldenAtMs": goldens, "durationMs": duration_ms}


def media(mid, path, w=1920, h=1080):
    return {"id": mid, "hintPath": path, "kind": "MEDIA_KIND_IMAGE", "naturalSize": {"width": w, "height": h}}


# ---- 1. title slide ----------------------------------------------------------

def title():
    title_rtf = rtf(
        [("\\qc", [("\\f0\\fs52\\expnd12\\cf1", "WELCOME TO")]),
         ("\\qc\\sl-1900\\slmult0", [("\\f1\\fs192\\cf2", "Sunday "), ("\\f1\\fs192\\cf3", "Gathering")])],
        ["AvenirNext-DemiBold", "AvenirNext-Heavy"],
        [(1, 0.86, 0.72), (1, 1, 1), (1, 0.72, 0.45)])
    slide = {
        "id": "title", "label": "Title",
        "background": {"media": {"mediaId": "dusk", "fit": "CONTENT_FIT_FILL"}},
        "elements": [
            text_el("headline", transform(160, 250, 1600, 420), rtf_b64=title_rtf),
            shape("lower-third", transform(460, 800, 1000, 120), fill=hexc("#101428", 0.78), radius=60,
                  stroke={"color": color(1, 1, 1, 0.35), "width": 2},
                  shadow={"color": color(0, 0, 0, 0.55), "offset": {"x": 0, "y": 14}, "blurRadius": 36}),
            text_el("time", transform(460, 800, 1000, 120),
                    runs=[run("10:30 AM  ·  Main Hall", "AvenirNext-DemiBold", 46, (1, 1, 1, 0.95))]),
        ],
    }
    doc = {"id": "title-doc", "media": [media("dusk", "media/dusk.jpg")],
           "presentations": [{"id": "welcome", "name": "Welcome", "slides": [slide]}]}
    return scene("title", "Image background (FILL), RTF title with tracking and exact line spacing, "
                 "a rounded lower third with stroke and soft shadow.", doc, [take(0, "welcome", "title")], [500], 3000)


# ---- 2. lyrics with dissolves ------------------------------------------------
# "Amazing Grace", John Newton (1779): public domain.

VERSES = [
    ("Verse 1", "Amazing grace! How sweet the sound\nThat saved a wretch like me!"),
    ("Verse 1 (2)", "I once was lost, but now am found;\nWas blind, but now I see."),
    ("Verse 2", "’Twas grace that taught my heart to fear,\nAnd grace my fears relieved;"),
]


def lyrics():
    slides = []
    for i, (label, text) in enumerate(VERSES):
        body = rtf([("\\qc\\sl-1500\\slmult0", [("\\f0\\fs120\\cf1", text)])], ["AvenirNext-DemiBold"], [(1, 1, 1)])
        s = {"id": f"s{i}", "label": label,
             "elements": [text_el("lyrics", transform(120, 240, 1680, 600), rtf_b64=body)]}
        if i == 0:
            s["background"] = {"color": hexc("#0d1b2a")}
            s["actions"] = [{"showFixtureId": "credit"}]
        slides.append(s)
    credit = {"id": "credit", "name": "Song credit", "z": 150, "elements": [
        shape("bar", transform(0, 990, 1920, 90), fill=color(0, 0, 0, 0.45)),
        text_el("credit-text", transform(60, 990, 1800, 90), align="TEXT_ALIGN_LEFT", runs=[
            run("Amazing Grace", "AvenirNext-DemiBold", 30, (1, 0.8, 0.45, 1)),
            run("   John Newton, 1779  ·  public domain", "AvenirNext-Regular", 30, (1, 1, 1, 0.8))]),
    ]}
    doc = {"id": "lyrics-doc", "fixtures": [credit],
           "presentations": [{"id": "amazing-grace", "name": "Amazing Grace", "slides": slides,
                              "defaultTransition": {"packageId": "core", "transitionId": "dissolve",
                                                    "durationMs": 1000, "easing": "EASING_EASE_IN_OUT"}}]}
    script = [take(0, "amazing-grace", "s0"), take(2000, "amazing-grace", "s1"), take(4000, "amazing-grace", "s2")]
    return scene("lyrics_dissolve", "Three lyric slides, 1 s ease-in-out dissolves (both sides offscreen, then a "
                 "crossfade), a credit fixture at z 150 shown by a slide action.", doc, script, [1200, 2500, 5200], 6000)


# ---- 3. blend modes ------------------------------------------------------------

MODES = ["NORMAL", "ADD", "MULTIPLY", "SCREEN", "OVERLAY", "DARKEN", "LIGHTEN", "SOFT_LIGHT", "DIFFERENCE"]


def blend_modes():
    els = []
    for i, mode in enumerate(MODES):
        col, row = i % 3, i // 3
        x, y = 330 + col * 470, 50 + row * 345
        els.append(shape(f"disc-{mode.lower()}", transform(x, y, 260, 260), kind="SHAPE_KIND_ELLIPSE",
                         fill=color(1.0, 0.55, 0.12), blendMode=f"BLEND_MODE_{mode}"))
        els.append(shape(f"tag-{mode.lower()}", transform(x - 20, y + 268, 300, 52), fill=color(0, 0, 0, 0.6), radius=26))
        els.append(text_el(f"label-{mode.lower()}", transform(x - 20, y + 268, 300, 52),
                           runs=[run(mode.replace("_", " ").title(), "Menlo-Regular", 26)]))
    doc = {"id": "blend-doc", "media": [media("spectrum", "media/spectrum.jpg")],
           "presentations": [{"id": "blend", "name": "Blend modes", "slides": [{
               "id": "grid", "label": "Blend modes",
               "background": {"media": {"mediaId": "spectrum", "fit": "CONTENT_FIT_FILL"}},
               "elements": els}]}]}
    return scene("blend_modes", "The same orange disc in all nine blend modes over a hue/value backdrop. NORMAL and ADD "
                 "use fixed-function blending; the rest read a backdrop copy.", doc, [take(0, "blend", "grid")], [100], 2000)


# ---- 4. 3D depth -----------------------------------------------------------------

CARDS = [("Verse", "#e76f51"), ("Chorus", "#f4a261"), ("Bridge", "#2a9d8f"), ("Tag", "#457b9d"), ("Outro", "#8d5fd3")]


def depth():
    # A floor plane: positive X rotation lays it back, centred deep enough to sort behind every card.
    els = [shape("floor", transform(-600, -480, 3120, 2600, z=-700, rotation_x_deg=85), fill=hexc("#232838"))]
    for i, (name, hx) in enumerate(CARDS):
        k = i - 2
        rot = {"rotation_y_deg": -38 * k / max(1, abs(k)) if k else 0}
        x, z = 760 + k * 330, (220 if k == 0 else -160 * abs(k))
        t = transform(x, 250, 400, 540, z=z, **rot)
        els.append(shape(f"card-{name.lower()}", t, fill=hexc(hx), radius=36,
                         stroke={"color": color(1, 1, 1, 0.8), "width": 4},
                         shadow={"color": color(0, 0, 0, 0.5), "offset": {"x": 0, "y": 18}, "blurRadius": 40}))
        els.append(text_el(f"card-label-{name.lower()}", transform(x, 250, 400, 540, z=z + 0.5, **rot),
                           runs=[run(name, "AvenirNext-Heavy", 64)]))
    els.append(text_el("crawl", transform(160, 850, 1600, 200, z=100, rotation_x_deg=60),
                       runs=[run("z is real depth: 0 is the base plane, positive comes toward you.",
                                 "AvenirNext-DemiBold", 44, (1, 0.9, 0.5, 1))]))
    doc = {"id": "depth-doc", "defaultPerspective": {"distance": 1800},
           "presentations": [{"id": "depth", "name": "Depth", "slides": [{
               "id": "flow", "label": "Cover flow", "background": {"color": hexc("#14161f")}, "elements": els}]}]}
    return scene("depth_3d", "Cards at several depths and Y rotations, a tilted floor and tilted text, through a "
                 "1800-unit perspective camera; draw order is by projected depth.", doc, [take(0, "depth", "flow")], [100], 2000)


# ---- 5. text layout -------------------------------------------------------------

PROSE = ("Text flows inside any container path, minus exclusions it wraps around. The engine and the editor share "
         "one layout library, so the line breaks you see while editing are the line breaks on the output. ")


def text_layout():
    circle_text = rtf([("\\qc\\sl-560\\slmult0", [("\\f0\\fs40\\cf1", PROSE * 3)])], ["Georgia"], [(0.15, 0.12, 0.1)])
    wrap_text = rtf([("\\qj\\sa200", [("\\f0\\fs40\\cf1", PROSE * 3)]),
                     ("\\qj", [("\\f1\\fs36\\cf2", "Exclusions are just paths in box coordinates.")])],
                    ["AvenirNext-Regular", "AvenirNext-DemiBold"], [(0.95, 0.95, 0.95), (1, 0.75, 0.4)])
    shrink = rtf([("\\qc", [("\\f0\\fs160\\cf1", "Shrink to fit keeps every word on screen, however long the line gets.")])],
                 ["AvenirNext-Heavy"], [(1, 1, 1)])
    els = [
        shape("circle", transform(80, 200, 640, 640), kind="SHAPE_KIND_ELLIPSE", fill=hexc("#f1e3c8"),
              shadow={"color": color(0, 0, 0, 0.45), "offset": {"x": 0, "y": 10}, "blurRadius": 30}),
        text_el("circle-text", transform(80, 200, 640, 640), rtf_b64=circle_text, valign="VERTICAL_ALIGN_MIDDLE",
                container=ellipse_path(640, 640)),
        shape("panel", transform(800, 120, 1040, 520), fill=color(1, 1, 1, 0.07), radius=24),
        shape("obstacle", transform(1240, 250, 240, 240), kind="SHAPE_KIND_ELLIPSE", fill=hexc("#2a9d8f")),
        text_el("wrap-text", transform(800, 120, 1040, 520), rtf_b64=wrap_text, valign="VERTICAL_ALIGN_TOP",
                padding={"top": 30, "left": 36, "bottom": 30, "right": 36},
                # The obstacle's circle, grown 16 units for a margin, in box coordinates.
                exclusions=[ellipse_path(272, 272, 1240 - 800 - 16, 250 - 120 - 16)]),
        shape("shrink-box", transform(800, 700, 1040, 220), fill=None,
              stroke={"color": color(1, 1, 1, 0.5), "width": 3}, radius=12),
        text_el("shrink-text", transform(800, 700, 1040, 220), rtf_b64=shrink, fit="TEXT_FIT_SHRINK_TO_FIT",
                padding={"top": 12, "left": 24, "bottom": 12, "right": 24}),
    ]
    doc = {"id": "text-doc", "presentations": [{"id": "text", "name": "Text layout", "slides": [{
        "id": "layout", "label": "Containers", "background": {"color": hexc("#1d2433")}, "elements": els}]}]}
    return scene("text_layout", "Text in a circular container path, justified text wrapping around an exclusion, and "
                 "shrink-to-fit.", doc, [take(0, "text", "layout")], [100], 2000)


if __name__ == "__main__":
    for i, s in enumerate([title(), lyrics(), blend_modes(), depth(), text_layout()], 1):
        path = HERE / f"{i:02d}_{s['name']}.scene.json"
        path.write_text(json.dumps(s, indent=1, ensure_ascii=False) + "\n")
        print("wrote", path.name)
