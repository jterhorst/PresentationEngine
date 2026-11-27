"""Contract checks: protobuf round-trips and JSON Schema validation.

Usage: python3 tests/validate.py <generated-python-dir>
(generate with: python3 -m grpc_tools.protoc -I proto --python_out=<dir> proto/engine/v1/*.proto)
"""
import copy
import json
import pathlib
import sys

from jsonschema import Draft202012Validator

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, sys.argv[1])

from engine.v1 import envelope_pb2, session_pb2, document_pb2, command_pb2, plugin_pb2  # noqa: E402
from library.v1 import library_pb2  # noqa: E402

failures = 0


def check(name, ok, detail=""):
    global failures
    print(("PASS " if ok else "FAIL ") + name + (f" ({detail})" if detail and not ok else ""))
    failures += 0 if ok else 1


# --- Protobuf round-trips -------------------------------------------------
hello = envelope_pb2.ClientMessage(request_id=1)
hello.hello.protocol.major = 1
hello.hello.kind = session_pb2.CLIENT_KIND_UI
hello.hello.display_name = "Booth Mac"
hello.hello.capabilities.extend(["preview.iosurface", "document.patch"])
back = envelope_pb2.ClientMessage.FromString(hello.SerializeToString())
check("ClientMessage Hello round-trip", back == hello and back.WhichOneof("body") == "hello")

take = envelope_pb2.ClientMessage(request_id=2)
take.command.take_slide.presentation_id = "p1"
take.command.take_slide.slide_id = "s3"
take.command.take_slide.transition_override.package_id = "org.example.basics"
take.command.take_slide.transition_override.transition_id = "luma_wipe"
take.command.take_slide.transition_override.duration_ms = 800
back = envelope_pb2.ClientMessage.FromString(take.SerializeToString())
check("ClientMessage TakeSlide round-trip", back == take and back.command.take_slide.HasField("transition_override"))

doc = document_pb2.Document(id="d1")
doc.canvas.width, doc.canvas.height = 1920, 1080
pres = doc.presentations.add(id="p1", name="Sunday")
slide = pres.slides.add(id="s1", label="Verse 1")
el = slide.elements.add(id="e1", name="Lyrics")
el.text.runs.add(text="Next up: {slide.next.label}")
el.mask.media_id = "m-mask"
el.mask.channel = document_pb2.MASK_CHANNEL_LUMA
fx = el.effects.add(id="fx1", package_id="org.example.basics", effect_id="glow", enabled=True)
fx.params["radius"].number = 16
check("Slide has no background unless set", not slide.HasField("background"))
load = envelope_pb2.ClientMessage(request_id=3)
load.load_document.document.CopyFrom(doc)
back = envelope_pb2.ClientMessage.FromString(load.SerializeToString())
check("LoadDocument round-trip", back.load_document.document == doc)

hb = envelope_pb2.EngineMessage(seq=10)
hb.heartbeat.engine_time_ns = 123_000_000
ph = hb.heartbeat.playheads.add(element_id="bg", media_id="m1", position_ns=5_000_000_000,
                                sampled_at_ns=123_000_000, rate=1.0)
oh = hb.heartbeat.outputs.add(output_id="sdi-1", measured_fps=59.94)
back = envelope_pb2.EngineMessage.FromString(hb.SerializeToString())
check("EngineMessage Heartbeat round-trip", back == hb)

pm = envelope_pb2.ClientMessage(request_id=4)
batch = pm.plugin.scene_edits
fix = batch.edits.add().show_fixture
fix.id = "plugin:org.example.timers/t1"
fix.z = 150
fix.elements.add(id="digits").text.runs.add(text="{plugin:org.example.timers/t1|mm:ss}")
bv = pm.plugin  # separate message for bindings
bind_msg = envelope_pb2.ClientMessage(request_id=5)
cd = bind_msg.plugin.publish_bindings.values["t1"].countdown
cd.target_ns, cd.running = 5_400_000_000_000, True
for m in (pm, bind_msg):
    check(f"Plugin message round-trip ({m.plugin.WhichOneof('body')})",
          envelope_pb2.ClientMessage.FromString(m.SerializeToString()) == m)

# --- Data model: text layout, positions, extensions -------------------------
from engine.v1 import common_pb2  # noqa: E402

el = document_pb2.Element(id="lyrics")
el.transform.position.x, el.transform.position.y, el.transform.position.z = 160, 700, 2.0
el.transform.size.x, el.transform.size.y = 1600, 300
el.text.rtf = rb"{\rtf1\ansi{\fonttbl\f0 Helvetica;}\f0\fs96 Amazing {\b grace}\par}"
el.text.plain_text = "Amazing grace"
path = el.text.container
path.commands.add().move_to.CopyFrom(common_pb2.Vec2(x=0, y=0))
path.commands.add().line_to.CopyFrom(common_pb2.Vec2(x=1600, y=0))
c = path.commands.add().cubic_to
c.control1.x, c.control1.y, c.control2.x, c.control2.y, c.to.x, c.to.y = 1600, 200, 800, 300, 0, 300
path.commands.add().close.SetInParent()
el.text.exclusions.add().commands.add().move_to.CopyFrom(common_pb2.Vec2(x=700, y=100))
ext = el.extensions["org.example.lighting"]
ext.schema_version = 2
ext.data.update({"cue": "12.5", "fade_s": 3, "fixtures": ["front", "back"]})
back = document_pb2.Element.FromString(el.SerializeToString())
check("Element with RTF, container path, z and extension round-trips", back == el)
check("Text container is optional", not document_pb2.TextElement().HasField("container"))
check("Extension Struct keeps nested data",
      back.extensions["org.example.lighting"].data["fixtures"][1] == "back")

sh = document_pb2.Element(id="banner")
sh.transform.position.z = -200
sh.transform.rotation_y_deg = 25
sh.shape.kind = document_pb2.SHAPE_KIND_PATH
sh.shape.path.commands.add().move_to.CopyFrom(common_pb2.Vec2(x=0, y=0))
sh.shape.path.commands.add().line_to.CopyFrom(common_pb2.Vec2(x=800, y=40))
sh.shape.path.commands.add().close.SetInParent()
sh.shape.fill.r, sh.shape.fill.g, sh.shape.fill.b, sh.shape.fill.a = 0.1, 0.2, 0.6, 0.75
sh.shape.stroke.color.a, sh.shape.stroke.width = 1.0, 6
sh.shape.stroke.alignment = document_pb2.STROKE_ALIGNMENT_OUTSIDE
sh.shape.stroke.dash_pattern.extend([12, 6])
outer = sh.shape.shadows.add(blur_radius=24, spread=2)
outer.offset.y = 8
outer.color.a = 0.5
sh.shape.shadows.add(inner=True, blur_radius=6).color.a = 0.3
back = document_pb2.Element.FromString(sh.SerializeToString())
check("Path shape with fill alpha, dashed outside stroke, outer+inner shadows round-trips", back == sh)
check("Shape fill is optional (unset = no fill)", not document_pb2.ShapeElement().HasField("fill"))
persp = document_pb2.Slide(id="s3d")
persp.perspective.distance = 2400
check("Slide perspective is optional and round-trips",
      not document_pb2.Slide().HasField("perspective")
      and document_pb2.Slide.FromString(persp.SerializeToString()).perspective.distance == 2400)

pe = document_pb2.Element(id="timer1")
pe.plugin.plugin_id = "org.example.timers"
pe.plugin.type = "timer_display"
pe.plugin.content.schema_version = 1
pe.plugin.content.data.update({"timer_id": "t1", "format": "mm:ss"})
pe.plugin.fallback.add(id="fb").text.runs.add(text="30:00")
check("PluginElement round-trip", document_pb2.Element.FromString(pe.SerializeToString()) == pe)

# Unknown-field preservation: a newer writer adds field 60 to a Slide; an older
# reader must re-serialize it untouched.
slide = document_pb2.Slide(id="s1", label="Chorus")
slide.extensions["org.example.timers"].data.update({"auto_start": True})
tag = 60 << 3 | 2                                   # field 60, length-delimited
future_field = bytes([(tag & 0x7F) | 0x80, tag >> 7, 5]) + b"hello"
round_tripped = document_pb2.Slide.FromString(slide.SerializeToString() + future_field).SerializeToString()
check("Unknown fields survive an older reader", future_field in round_tripped)

prop = envelope_pb2.EngineMessage(seq=11)
upd = prop.extension_proposal.updates.add(entity_id="s1")
upd.set.schema_version = 1
upd.set.data.update({"auto_start": False})
prop.extension_proposal.plugin_id = "org.example.timers"
check("ExtensionProposal round-trip", envelope_pb2.EngineMessage.FromString(prop.SerializeToString()) == prop)


# --- Library model ---------------------------------------------------------
lib_doc = library_pb2.DocumentFile(model_version=1)
lib_doc.presentation.id, lib_doc.presentation.name = "amazing-grace", "Amazing Grace"
lib_doc.presentation.slides.add(id="v1", label="Verse 1")
lib_doc.media.add(id="m1", content_hash="sha256:ab", kind=document_pb2.MEDIA_KIND_VIDEO)
check("DocumentFile round-trip", library_pb2.DocumentFile.FromString(lib_doc.SerializeToString()) == lib_doc)
ws = library_pb2.Workspace(model_version=1, layout_id="core.default")
ws.libraries.add(library_id="lib1", path="~/Presentations/Worship")
root = ws.playlists
root.name = "Playlists"
services = root.folders.add(id="f1", name="2026 Services")
pl = services.playlists.add(id="pl1", name="Sunday, Nov 30")
pl.items.add(id="i1", presentation_id="amazing-grace")
pl.items.add(id="i2", header="Sermon")
check("Workspace with nested folders and playlists round-trips",
      library_pb2.Workspace.FromString(ws.SerializeToString()) == ws)

# --- JSON Schemas ----------------------------------------------------------
def load(p):
    return json.loads((ROOT / p).read_text())


schemas = {name: load(f"schemas/{name}.schema.json") for name in ("gpu-package", "logic-plugin", "ui-schema", "workspace-layout")}
for name, s in schemas.items():
    Draft202012Validator.check_schema(s)
    check(f"schema {name} is valid JSON Schema 2020-12", True)


def validate(schema, doc, name, expect_ok=True):
    errors = sorted(Draft202012Validator(schemas[schema]).iter_errors(doc), key=lambda e: e.path)
    ok = (not errors) == expect_ok
    detail = "; ".join(f"{list(e.path)}: {e.message[:120]}" for e in errors[:3]) if expect_ok else "accepted"
    check(name, ok, detail)


gpu = load("examples/gpu-package/org.example.basics/package.json")
validate("gpu-package", gpu, "example GPU package validates")
bad = copy.deepcopy(gpu); bad["effects"][0]["params"][0]["id"] = "Radius-1"
validate("gpu-package", bad, "GPU package rejects non-identifier param id", expect_ok=False)
bad = copy.deepcopy(gpu); bad["transitions"][0]["shader"]["file"] = "../../etc/passwd"
validate("gpu-package", bad, "GPU package rejects path traversal", expect_ok=False)
bad = copy.deepcopy(gpu); bad["sources"][0]["params"][3].pop("choices")
validate("gpu-package", bad, "GPU package requires choices for choice params", expect_ok=False)
bad = copy.deepcopy(gpu); bad["effects"][0]["shader"] = {"file": "x.slang", "entry": "main"}
validate("gpu-package", bad, "GPU package rejects both shader and passes", expect_ok=False)

plugin = load("examples/logic-plugin/org.example.timers/plugin.json")
validate("logic-plugin", plugin, "example logic plugin validates")
bad = copy.deepcopy(plugin); bad["capabilities"].append("root")
validate("logic-plugin", bad, "logic plugin rejects unknown capability", expect_ok=False)

for f in ("ui.json", "settings-ui.json"):
    validate("ui-schema", load(f"examples/logic-plugin/org.example.timers/{f}"), f"timers {f} validates")
ui = load("examples/logic-plugin/org.example.timers/ui.json")
bad = copy.deepcopy(ui); bad["panels"][0]["layout"]["children"][1]["type"] = "frobnicator"
validate("ui-schema", bad, "UI schema rejects unknown control type", expect_ok=False)
bad = copy.deepcopy(ui); bad["panels"][0]["layout"]["children"][0]["bind"] = "timers/0"
validate("ui-schema", bad, "UI schema rejects malformed bind path", expect_ok=False)
bad = copy.deepcopy(ui); del bad["panels"][0]["layout"]["children"][1]["action"]
validate("ui-schema", bad, "UI schema requires an action on buttons", expect_ok=False)
bad = copy.deepcopy(ui); del bad["inspectors"][0]["applies_to"]
validate("ui-schema", bad, "UI schema requires applies_to on inspectors", expect_ok=False)
validate("ui-schema", {"schema": 1, "inspectors": ui["inspectors"]}, "UI schema allows inspectors without a panel")
validate("ui-schema", {"schema": 1}, "UI schema rejects a file with neither panel nor inspectors", expect_ok=False)
bad = copy.deepcopy(plugin); bad["capabilities"].remove("elements.render")
validate("logic-plugin", bad, "element_types require the elements.render capability", expect_ok=False)


bad = copy.deepcopy(ui); bad["panels"][0]["placements"] = ["toolbar"]
validate("ui-schema", bad, "plugin panels can't claim the toolbar", expect_ok=False)

# --- Workspace layouts -----------------------------------------------------
layout = load("examples/layouts/default.layout.json")
validate("workspace-layout", layout, "default layout validates")
bad = copy.deepcopy(layout); bad["panes"]["sidebar"]["panels"].append({"panel": "timers"})
validate("workspace-layout", bad, "layout rejects malformed panel refs", expect_ok=False)
bad = copy.deepcopy(layout); del bad["panes"]["playback"]
validate("workspace-layout", bad, "layout requires every pane", expect_ok=False)
bad = copy.deepcopy(layout); del bad["panes"]["edit"]
validate("workspace-layout", bad, "layout requires the Edit mode area", expect_ok=False)

# The load-time check the app performs: every panel exists (or is skipped) and
# sits in a placement it allows. Core placements are fixed by the app.
CORE_PLACEMENTS = {
    "core.search": {"toolbar"}, "core.engine_status": {"toolbar"}, "core.mode_switch": {"toolbar"},
    "core.drawer_toggle": {"toolbar"}, "core.slide_navigator": {"edit"},
    "core.clear_controls": {"toolbar", "playback"}, "core.libraries": {"sidebar"},
    "core.playlists": {"sidebar"}, "core.document_view": {"main"}, "core.slide_editor": {"edit"},
    "core.inspector": {"edit"}, "core.output_previews": {"playback"}, "core.live_layers": {"playback", "sidebar"},
}
plugin_panels = {f"plugin:{plugin['id']}/{p['id']}": set(p["placements"]) for p in ui["panels"]}


def lint_layout(lay):
    problems, skipped = [], []
    panes = lay["panes"]
    placed = [("toolbar", r) for k in ("leading", "center", "trailing") for r in panes.get("toolbar", {}).get(k, [])]
    placed += [(pane, slot["panel"]) for pane in ("sidebar", "main", "playback", "edit") for slot in panes[pane]["panels"]]
    placed += [("drawer", r) for r in panes["drawer"]["panels"]]
    for pane, ref in placed:
        allowed = CORE_PLACEMENTS.get(ref) or plugin_panels.get(ref)
        if allowed is None:
            skipped.append(ref)
        elif pane not in allowed:
            problems.append(f"{ref} not allowed in {pane}")
    return problems, skipped


problems, skipped = lint_layout(layout)
check("default layout places every panel where it's allowed", not problems and not skipped, str(problems + skipped))
bad = copy.deepcopy(layout); bad["panes"]["sidebar"]["panels"].append({"panel": "core.output_previews"})
check("layout lint catches a panel in a disallowed pane", lint_layout(bad)[0] == ["core.output_previews not allowed in sidebar"])
bad = copy.deepcopy(layout); bad["panes"]["edit"]["panels"].append({"panel": "core.output_previews"})
check("playback panels can't move into the Edit mode area", lint_layout(bad)[0] == ["core.output_previews not allowed in edit"])
bad = copy.deepcopy(layout); bad["panes"]["drawer"]["panels"].append("plugin:org.example.missing/panel")
check("layout lint skips panels from missing plugins", lint_layout(bad) == ([], ["plugin:org.example.missing/panel"]))

print(f"\n{'ALL PASSED' if failures == 0 else f'{failures} FAILED'}")
sys.exit(1 if failures else 0)
