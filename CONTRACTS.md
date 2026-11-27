# Engine contracts

These are the contracts between the engine and everything that talks to it: UI clients, logic plugins, GPU packages and native modules. The engine is a black box behind them. If a client or plugin needs something these contracts can't express, change the contract (with a version bump where required). Don't reach around it.

| Contract | Who uses it | Files |
| --- | --- | --- |
| Engine protocol | UI clients, tools, logic plugins | `proto/engine/v1/*.proto` |
| Native module ABI | DeckLink, NDI and other I/O modules | `include/engine/module.h` |
| GPU package | Effects, transitions, procedural sources | `schemas/gpu-package.schema.json`, `shaders/engine_effect.slang` |
| Logic plugin manifest | Timers, integrations, content providers | `schemas/logic-plugin.schema.json` |
| Declarative UI | Plugin panels, inspectors and settings, rendered by every client | `schemas/ui-schema.schema.json` |
| Workspace layout | Which panels go where in the main window | `schemas/workspace-layout.schema.json` |
| Library model | Libraries, documents, playlist folders (UI-owned files) | `proto/library/v1/library.proto` |
| Import adapters | Converters from .docx, .pages, .txt, .pro... into documents | `proto/importer/v1/importer.proto`, `importers` in the plugin manifest |

Examples: `examples/modules/null_output.c` (a working output module), `examples/gpu-package/org.example.basics/`, `examples/logic-plugin/org.example.timers/`.

---

## 1. Engine protocol

### Transports and framing

Every frame is exactly one serialized `ClientMessage` (client → engine) or `EngineMessage` (engine → client).

| Transport | Used by | Framing |
| --- | --- | --- |
| XPC (macOS) | Local UI clients | One XPC dictionary per frame: key `pb` holds the bytes. Preview and thumbnail frames may carry a `surface` key with an IOSurface (mach port); `LocalSurface.surface_id` says which pooled surface it is. |
| Unix domain socket | Logic plugin hosts, local tools | 4-byte big-endian length, then the bytes. Socket path in `ENGINE_PLUGIN_SOCKET`. |
| WebSocket | Headless, remote and browser clients | One binary WebSocket message per frame, subprotocol `engine.v1`. `wss://` with pairing for anything not on localhost. |

The maximum frame size is 16 MiB. Media uploads use 1 MiB chunks.

### Versioning

- `ProtocolVersion.major` changes only for incompatible changes. On a major mismatch the engine replies `ERROR_CODE_VERSION_MISMATCH` and closes the connection.
- Within a major version, changes are additive only. Never reuse or renumber a field. Mark removed fields `reserved`. Unknown fields and enum values must be ignored by readers.
- CI runs a Protobuf breaking-change check against the last release on every PR.
- Optional features are negotiated through **capabilities** (below), not version numbers.

### Handshake

1. The client connects and sends `Hello` (protocol version, kind, display name, capabilities, plus `resume_token` if reconnecting and `pairing_credential` if remote).
2. The engine replies `Welcome` with the role, negotiated capabilities, heartbeat interval and control seat state.
3. The engine then sends `DocumentSnapshot` (if a document is loaded), `StateSnapshot`, `PluginCatalog`, and every plugin's `PluginUi` and `PluginState`.
4. Heartbeats start.

Role assignment: the first UI to connect becomes controller. A client presenting a valid `resume_token` for a RESERVED seat gets controller back. Everyone else is an observer. Plugin hosts are always `ROLE_PLUGIN`.

### Requests and replies

A client sets `request_id` (nonzero, unique per connection) when it wants an answer. The engine answers with `reply_to = request_id`. Usually that's a `Reply` (ok or error); some requests get a typed answer instead:

| Request | Answer |
| --- | --- |
| `LoadDocument`, `PatchDocument` | `Reply.ok.revision`; then `DocumentSnapshot` to everyone |
| `Command`, `IoConfigure`, `SettingsSet`, `PluginAction` | `Reply` when validated; effects arrive as `StateEvent` / `PluginState` |
| `StateSnapshotRequest` | `StateSnapshot` |
| `ThumbnailRequest` | `Thumbnail` |
| `MediaQuery` | `MediaQueryResult` |
| `MediaUploadBegin` | `Reply.ok.id` = upload id |
| `SettingsGet` | `SettingsValues` |
| `PreviewSubscribe` | `Reply`; then `PreviewFrame`s |

`EngineMessage.seq` increases by one per message on a connection, which makes dropped messages detectable when debugging.

### Who may send what

| Messages | Controller | Observer | Plugin |
| --- | --- | --- | --- |
| `LoadDocument`, `PatchDocument`, `Command`, `IoConfigure`, `SettingsSet`, `PluginAction`, `ReleaseControl` | yes | no (`NOT_CONTROLLER`) | no |
| `ControlRequest`, `ControlPinSubmit` | no | yes | no |
| `StateSnapshotRequest`, `PreviewSubscribe`, `ThumbnailRequest`, `MediaQuery`, `SettingsGet` | yes | yes | no |
| Media upload | yes | no | no |
| `PluginMessage` | no | no | yes, within its manifest capabilities |

### Control handoff

1. An observer sends `ControlRequest`.
   - If the seat is **VACANT**, the observer becomes controller at once, with no PIN.
   - Otherwise the engine sends `ControlPinIssued` (6 digits, single use, short expiry) **only to the controller**.
2. The controller's operator tells the requester the PIN out loud. The requester sends `ControlPinSubmit`.
3. On a match, roles swap atomically and every client gets `RoleChanged`. Wrong PINs return `PIN_INVALID`. Too many attempts return `PIN_LOCKED_OUT` and the controller gets `ControlRequestEnded`.
4. If the controller disconnects, the seat is **RESERVED** for its resume token for a configurable grace period, then becomes **VACANT**.
5. A local admin command on the engine machine can always take control.

### Documents and revisions

- `LoadDocument` replaces the document. The engine assigns `revision` and ignores any value the client sends.
- `PatchDocument` (capability `document.patch`) upserts or removes top-level entities against `base_revision`. A wrong base returns `STALE_REVISION`.
- Media must exist before a document references it: run `MediaQuery`, upload what's missing, then load. Otherwise the reply is `MEDIA_MISSING`.
- **Edits never re-display.** If the live slide changes in a new revision, the engine sends `StateEvent.live_slide_stale_changed` with `stale = true`. Only `Command.retake` re-displays it.

### State sync

- `StateSnapshot` is the full live state. `StateEvent`s are ordered deltas, each with `state_revision` = previous + 1 and the engine time it took effect (always a frame edge).
- If a client sees a gap, or a `Heartbeat.state_revision` ahead of its own, it sends `StateSnapshotRequest` and replaces its copy.
- Commands apply atomically at the next frame edge, never mid-frame.

### Heartbeat and liveness

- The engine sends `Heartbeat` every `Welcome.heartbeat_interval_ms`, carrying engine time, revisions, output, input and plugin health, and media playheads.
- Playheads are `(position_ns, sampled_at_ns, rate)`. Clients interpolate locally for smooth scrubbers.
- A client that misses `missed_heartbeats_before_disconnect` heartbeats shows "engine disconnected" and reconnects with its resume token.
- The engine treats a client as gone when its transport closes. XPC and sockets report that promptly; WebSocket uses ping/pong.

### Previews

- `PreviewSubscribe` per output, with max size, max fps and transport. The engine renders one preview per output and size bucket, shared by every subscriber.
- **IOSurface** (local): surfaces come from a small pool shared over XPC. `PreviewFrame.surface` names the one holding the newest frame. A client must not hold a surface longer than one frame interval.
- **H.264** (remote): low-latency access units; keyframes carry `codec_config`. Encoding runs only while someone is subscribed.

### Capabilities

Strings exchanged in `Hello` / `Welcome`. A feature is used only if both sides list it.

| Capability | Meaning |
| --- | --- |
| `preview.iosurface` | Can receive IOSurface previews (local XPC only) |
| `preview.h264` | Can receive H.264 previews |
| `document.patch` | Uses `PatchDocument` |
| `media.local_path` | Engine may read `MediaRef.hint_path` directly when the hash matches |
| `thumbnails.png` | Wants PNG thumbnails instead of surfaces |

---

## 2. Data model and extensions

The document model lives in `document.proto`. It's the same structure the controller sends over the wire and saves to disk.

```
Document
├── Playlist ── PlaylistItem (presentation | media cue | header)
├── Presentation ── Slide ── Element (text | shape | media | live input | source | plugin)
├── FixtureDef ── Element
├── StageLayout ── Element
└── MediaRef
```

### Positions and 3D depth

Each container (slide, fixture, plugin layer, stage layout) is its own 3D space viewed through a camera. Containers still stack against each other by layer z (background 0, slide 100, fixtures any integer) and never interleave.

- `Transform.position` is `(x, y, z)` in canvas units: x right, y down, **z toward the viewer**. **z = 0 is the base plane**, where an element appears exactly as authored in 2D. Positive z comes closer and appears larger; negative z recedes.
- Rotation pivots on `anchor`. Rotations apply after scale, in this order: `rotation_deg` (around the view axis, clockwise on screen), then `rotation_y_deg` (positive turns the right edge away), then `rotation_x_deg` (positive tilts the top edge away).
- **Camera:** `Perspective { distance, vanishing_point, orthographic }`. Lookup order: the container's own setting, then `Document.default_perspective`, then the engine default (distance = canvas width, vanishing point = canvas center).
- **Projection:** for a transformed corner `(X, Y, Z)`, screen = `vp + (X − vp, Y − vp) × d / (d − Z)`. Orthographic uses a factor of 1.
- **Draw order:** back to front by the depth of each element's center. Equal depth keeps list order. Intersecting elements aren't split; they draw in that order.
- **Culling:** an element with any corner at `Z ≥ 0.99 × d` (at or behind the camera) isn't drawn.
- **What's 3D.** Text layout, masks, effects and shadows happen in the element's flat local space; the result is then projected, so they tilt with the element. Transitions act on whole layers after projection.
- **Cost:** an element at z = 0 with no X/Y rotation takes the 2D fast path. 3D elements draw as a perspective-correct textured quad with mipmapped sampling.
- **One implementation of the math.** `tests/reference/projection.py` is the spec, and `projection_cases.json` holds its expected corners. The engine, the shared editor library and the web editor must match it within 1e-3 canvas units.

### Shapes

- `kind`: rectangle (with `corner_radius`), ellipse, or path (`Path` in box coordinates).
- **Fill:** a color whose alpha is the fill opacity. Unset means no fill.
- **Stroke** (the path outline): color with alpha, thickness, alignment (center, inside, outside), join, cap, dash pattern and phase, and miter limit. Unset or a width of 0 means no stroke.
- **Shadows:** any number, each with color and alpha, offset, blur radius, spread, and outer or inner.
- **Paint order:** outer shadows, fill, inner shadows, stroke. Then the element's opacity, blend mode, mask and effects apply to the whole result.

### Text content and layout

- **RTF is the source of truth** for styled text (`TextElement.rtf`). `runs` is a simpler form used only when `rtf` is empty, e.g. for text built by plugins without an RTF writer. `plain_text` is a UI-written mirror for search and plugins; the engine derives its own and never trusts it for rendering.
- **Supported RTF subset** (rendered identically by engine and editor): `\rtf1`, `\ansi` / `\ansicpg`, `\uN` Unicode escapes, `\fonttbl` + `\f`, `\fs`, `\b`, `\i`, `\ul`, `\strike`, `\caps`, `\colortbl` + `\cf`, `\highlight` (color table index), `\ql` `\qc` `\qr` `\qj`, `\sl` + `\slmult`, `\sb` / `\sa`, `\expnd` / `\expndtw`, `\par`, `\line`. Other control words are ignored when rendering but kept in the stored bytes.
- **Parity.** The shared C++ text library parses and lays out this subset for the engine and the native editor. Compile it to WebAssembly so the web UI reads and writes RTF the same way.
- **Container.** Text flows inside `container`, a `Path` in box coordinates (any shape: circles, angled banners, irregular blocks), minus the `exclusions` paths it wraps around. With no container, the box inset by `padding` is the container. `vertical_align` and `TEXT_FIT_SHRINK_TO_FIT` work against the container's bounds.
- **Bindings in RTF.** A binding is plain text inside the RTF content, e.g. `{slide.next.text}`. Its resolved value takes the style of the binding's first character. Literal braces are escaped in RTF as usual (`\{`, `\}`).
- `ShapeElement` with `SHAPE_KIND_PATH` uses the same `Path` type.

### Every entity has the same three extension points

| Field | Owner | Use |
| --- | --- | --- |
| `id` | Whoever creates it | Stable, unique in the document (UUIDs). Plugins and patches target entities by id. |
| `metadata` (field 50) | The operator | Plain key/value strings the operator can see and edit, such as CCLI number or song key. Visible to plugins. |
| `extensions` (field 51) | A plugin or app, keyed by its id | Structured data the owner persists: `Extension { schema_version, data (JSON-like Struct), binary }`. Opaque to everyone else. |

`extensions` covers anything a plugin wants to remember about an entity: "start timer t1 when this slide is taken", a scripture reference, a lighting cue, the UI's own layout hints (keyed by the UI's app id).

### Plugin element types

When a plugin needs a new *kind* of element (a scripture block, a timer display, a lower third fed from a spreadsheet), it declares `element_types` in its manifest and the document stores a `PluginElement { plugin_id, type, content, fallback }`.

- After every document load or patch, the engine sends the plugin `ResolvePluginElements` for elements whose content changed. The plugin answers with ordinary core elements, positioned relative to the element's box.
- The engine renders the resolved elements. It never asks the plugin at take time.
- The UI saves the latest resolved result as `fallback`. A document opened without the plugin still renders, and its data survives.

### Who writes extension data

The document belongs to the controller, and the engine never edits it. So:

- **Inspectors** (in the plugin's UI schema) edit an entity's extension data in the UI. The UI writes the change into its document and sends `PatchDocument`. No plugin code runs.
- **Plugins** send an `ExtensionProposal`. The engine checks every update touches only the plugin's own key or its own PluginElements, then forwards the proposal to the controller. The controller applies it with `PatchDocument`; UIs normally auto-apply proposals for the owner's own key and record them in undo history.
- Nobody writes another owner's key. The engine rejects proposals that try (`CAPABILITY_DENIED`), and UIs must not let inspectors bind outside their own plugin's data.

### Preservation rules (the important part)

1. **Round-trip everything.** The engine and every client keep `extensions`, `metadata`, `PluginElement`s and **unknown protobuf fields** byte-for-byte, even when the owning plugin isn't installed. A UI that edits a slide must not drop extension data it doesn't understand. Protobuf's C++ and Swift runtimes preserve unknown fields by default; clients must not convert through a lossy intermediate format.
2. **Owners migrate.** `schema_version` belongs to the owner. A plugin reading an older version migrates it and writes back the new version through a proposal. Readers must accept data from newer versions without crashing; ignoring it is fine.
3. **Size limits.** The defaults are 64 KiB per extension and 8 MiB of extension data per document. Above that, `LoadDocument` fails with `INVALID_ARGUMENT` and a `field_path`. Large assets belong in `MediaRef`s, not extensions.
4. **JSON-friendly data.** `Struct` numbers are doubles, so store 64-bit integers and exact decimals as strings.
5. **Saved files.** A document on disk is a `library.v1.DocumentFile` (binary Protobuf): one `Presentation` plus the `MediaRef`s it uses. Libraries and playlists use the other `library.v1` messages (see Library model below). `model_version` records the model version a file was written with. A newer file opened by an older app keeps its unknown fields when saved again.

### Library model (UI-owned)

The engine works on a **show**: the `engine.v1.Document` the controller sends with `LoadDocument`. Operators work with libraries and playlists, which only the UI knows about. They're defined in `library.v1` so the native and web UIs share one file format.

| UI term | Message | Notes |
| --- | --- | --- |
| Library | `Library` + `LibraryEntry` index | One folder per library: `library.pb`, `documents/<id>.doc`, `media/<sha256>.<ext>` |
| Document | `DocumentFile` | A `Presentation` and its media |
| Folder | `PlaylistFolder` | Nests; holds playlists |
| Playlist | `engine.v1.Playlist` | Items reference documents by id, never copies |
| Workspace | `Workspace` | Open libraries, the playlist tree, the active layout |

**Building the show.** When the operator opens a playlist, the controller sends a show containing that playlist, every document it references, and the union of their media. If the operator takes a slide from a library document that isn't in the playlist, the controller adds that document to the show with a `PatchDocument` first. Edits to a document update the library file and are patched into the show; the live slide still waits for Retake.

### Evolving the core model

- Field numbers 1–49 are core fields. 50 and 51 are `metadata` and `extensions` on every entity. 100–199 are reserved for future core use.
- `Element.content` numbers 26–49 are kept for future core element types.
- Removing a core field means marking it `reserved`, never reusing its number.
- A plugin that becomes popular enough to deserve core fields gets them in a minor protocol version. Its old extension data stays readable, and the plugin migrates it.

---

## 3. Bindings

Text in any `TextElement` run may contain bindings, which the engine resolves **every frame**. This is how stage displays stay live through a UI crash and how timers count down with zero IPC.

```
binding  := "{" source [ "|" format ] "}"
literal braces: "{{" and "}}"
```

| Source | Value |
| --- | --- |
| `slide.current.text` / `slide.next.text` | All text runs of the slide, joined with line breaks (bindings inside are not expanded) |
| `slide.current.label` / `slide.next.label` | Slide label |
| `slide.current.notes` | Slide notes |
| `slide.current.metadata.<key>` | Slide metadata value |
| `presentation.name` | Live presentation name |
| `clock` | Local wall-clock time |
| `plugin:<plugin_id>/<key>` | `BindingValues.values[<key>]` published by that plugin |

| Value type | Formats | Default |
| --- | --- | --- |
| clock | Unicode date patterns, e.g. `h:mm a`, `HH:mm:ss` | `h:mm a` |
| countdown / stopwatch | `mm:ss`, `m:ss`, `h:mm:ss`, `ss`; negative countdowns get a leading `-` | `mm:ss` |
| number | `0`, `0.0`, `0.00` | as published |
| text | none | as published |

An unknown source renders as empty text and is reported once in the engine log.

---

## 4. Plugins

### Tiers

| Tier | Runs in | Can do | Contract |
| --- | --- | --- | --- |
| GPU package | The engine, as shaders and media only | Effects, transitions, sources | `gpu-package.schema.json` + `engine_effect.slang` |
| Logic plugin | Its own process | State, events, scene edits, bindings, UI | `logic-plugin.schema.json` + `plugin.proto` |
| Native module | The engine, as a dylib | Outputs and inputs | `module.h` |

### GPU packages

- A folder with `package.json`, Slang shaders and media. It contains no native code, so it can't crash the engine or access the system.
- Shaders compile at install time, are cached per GPU backend, and are rejected on compile failure.
- Each pass is a fragment shader over a fullscreen triangle. Inputs and output are **linear, premultiplied RGBA16F**. The engine applies blend modes, opacity and masks after the pass.
- Parameters bind by name: field names in `ConstantBuffer<Params> params` equal param ids. Package media binds as `Texture2D<float4> tex_<id>`.
- Multi-pass effects declare passes in order. A pass reads earlier passes as `pass:<id>` and may run at a smaller `scale`.
- The engine measures GPU time per effect instance. An instance over budget is bypassed rather than allowed to drop frames, and reported in `PluginHealth`.
- `engine_api` must equal the engine's `ENGINE_EFFECT_API`.

### Logic plugins

- The plugin host launches `run.command` and passes the engine socket in `ENGINE_PLUGIN_SOCKET`. The plugin speaks the same protocol as a UI, with `ClientKind PLUGIN_HOST` and its manifest in `Hello.plugin_manifest_json`.
- **Push scenes, not frames.** Plugins send declarative `SceneEditBatch`es and `BindingValues`. Nothing a plugin does needs per-frame messages.
- Each batch applies atomically at one frame edge, or is rejected whole.
- **Capabilities are enforced.** An edit outside the manifest's `capabilities` or `fixture_z_range` returns `CAPABILITY_DENIED`.
- **Namespacing.** Plugin fixture ids must start with `plugin:<plugin_id>/`. Injected element ids are prefixed by the engine. A plugin can only remove or update what it created.
- **Lifetimes.** Injected elements last until the slide changes unless `INJECT_LIFETIME_UNTIL_REMOVED`. Fixtures and layers last until the plugin removes them or the operator clears them.
- **Crashes.** If a plugin process exits, its fixtures, layers and binding values hold their last state. The host restarts it per `restart_policy`. On reconnect the plugin should republish UI, state and bindings.
- **Actions.** UI controls send `PluginAction`. The engine forwards it as `PluginActionInvoked`. The plugin answers by publishing new state, never through a reply.
- **Document access.** With `document.read` the plugin receives `DocumentSnapshot`s. With `elements.render` it defines element types and resolves `PluginElement`s. With `document.extensions` it may propose changes to its own extension data. See section 2.

### Declarative UI

- Clients render `ui-schema.schema.json` natively: SwiftUI on macOS, HTML on the web. The same schema describes engine and plugin settings.
- Controls **read** from plugin state through `bind` paths. A change **never writes** state directly: the client sends `PluginAction { action: control.action ?? "set", payload: { path, value } }`, or the control's own payload with `{"$value": true}` substituted. The plugin publishes new state and the control reflects it. State has one owner.
- Inside a `list`, paths starting with `.` are relative to the item, and `{"$item": "field"}` in a payload is replaced by that item's field.
- `readout` with `format: countdown | stopwatch` animates locally from heartbeat engine time.
- **Panels.** A plugin declares `panels`, each with an `id`, `title`, the `placements` it allows (`sidebar`, `drawer`, `playback`, `main`) and a `layout`. Layouts reference it as `plugin:<plugin_id>/<panel_id>`. A UI file with only a top-level `layout` counts as one panel, `main`, in the drawer. The toolbar is reserved for core panels for now.
- `inspectors` are panels for a selected entity (`applies_to`: slide, element, plugin_element, ...). Their bind paths read and write that entity's extension data; `$state.` paths read plugin state. The UI applies inspector edits to its document directly.
- State sync: `PluginState.snapshot_json` replaces state; `json_patch` (RFC 6902) applies to `state_version - 1`. On a gap, a client asks for settings or reconnects to get a fresh snapshot.

### Import adapters

An import adapter converts one outside file into library documents. It's one of any number: the app ships first-party adapters (.docx and .pages in v1), and any plugin can add more by listing `importers` in its manifest. A plugin may be importer-only, with no long-running process.

**Lifecycle**

1. The operator picks a file in the Open panel (File > Import, or drag onto a library). The panel's **Format** menu lists Automatic plus every adapter whose `file_types` match. Automatic picks by UTType, then extension, then the highest `priority`.
2. The panel's accessory view renders the selected adapter's **options form** (`options`, a UI schema with a top-level `layout`), seeded with `options_defaults`, plus an **Into library** picker.
3. The UI copies the file somewhere read-only, creates an empty output folder, and starts the adapter's `command`. It sends `Describe`, then one `ImportRequest` with the options as a JSON-like `Struct`.
4. The adapter streams `ImportProgress`, then a single `ImportResult` or `AdapterError`, and exits. Closing stdin or killing the process cancels.
5. The UI shows a **review sheet**: the documents, their slide counts and the warnings. The operator renames or unchecks documents and picks a template. Only then does anything get written to the library.

**Wire format.** Each message on stdin/stdout is a 4-byte big-endian length followed by one `AdapterRequest` or `AdapterResponse`. stderr goes to the import log.

**Output.** Adapters should return **semantic slides**: a label ("Verse 1"), a group, and text blocks by role (title, subtitle, body, footer) as RTF plus plain text. The operator's template decides fonts and layout. An adapter that truly knows layout, such as a ProPresenter importer, may return fully laid out `engine.v1.Slide`s instead. Extracted media goes in the output folder and is referenced by `local_id`; the UI moves it into the library's media folder by content hash.

**Templates.** A template is a library document with `metadata["template"] = "true"`. Its first slide's elements carry `metadata["role"]` (`title`, `subtitle`, `body`, `footer`). For each semantic slide, the UI copies the template slide and puts each text block into the element with the matching role. With "keep formatting" on, bold, italic and underline from the source RTF survive; the template supplies everything else.

**Provenance.** The UI records `metadata["import.source"]` (the original file name) on each imported document, and an extension under the adapter's plugin id with the adapter id, version and options used. That leaves room for Re-import later.

**Safety.** One process per import, with `timeout_s` (default 120 s). Adapters only read the input copy and write the output folder; they never talk to the engine or touch the library. Network access and app automation must be declared in `requires`, and the UI asks the operator the first time. A crash, timeout or malformed reply shows the error and writes nothing.

**Shared text heuristics** (both reference adapters, and expected of first-party ones):

- An empty paragraph or blank line ends a slide.
- If a Word document uses soft line breaks (Shift-Return) inside paragraphs, each paragraph becomes one slide instead, with an info warning.
- Heading paragraphs and lines like `Verse 1`, `[Chorus]` or `Bridge:` label the slides that follow.
- Lines starting with `CCLI` or `©` go to document metadata, not slides.
- `split_on` makes one document per file, per Heading 1 (e.g. a song set) or per page. Paragraph styles are matched by style name, so localized Word style ids still work.
- `lines_per_slide` splits long stanzas into numbered parts ("Verse 1 (2)").
- Documents with no content slides are dropped. Tables are flattened with a warning; images are reported but not imported by the reference .docx adapter.

**First-party adapters.** The v1 `.docx` and `.pages` adapters (`core.office-import`) live with the Mac app in [PresentationMacUI](https://github.com/jterhorst/PresentationMacUI), under `importers/core.office-import`. Their readers are a faithful port of pp7gen's input strategies (Apache-2.0, WorshipKit; see its NOTICE), verified against pp7gen's frozen fixtures. Pages files are read directly from their IWA archives, so the Pages app isn't required. See that folder's README for the verification results and what changed.

The example adapters (`examples/import-adapters/org.example.import-basics`) remain as small teaching references. All adapters here are Python for readability and testing; shipping builds port them to a native language behind the same protocol and tests.

### Workspace layouts

- A layout (`workspace-layout.schema.json`) lists panels per pane: `toolbar` (leading, center, trailing), `sidebar`, `main` (Show mode center), `edit` (Edit mode: replaces sidebar and main, panels left to right), `drawer` (Plugins menu order, the open panel, height) and `playback`, with widths, heights, collapsed state and pane visibility.
- **Playback is never replaced.** Edit mode swaps only the sidebar and main panes, so live output stays visible. Drawers open at the bottom of the center pane, or of the edit area in Edit mode, from the toolbar's Plugins menu (`core.drawer_toggle`).
- **Load-time check.** Each panel must be allowed in its pane. Plugin panels declare their own placements; core panels are fixed:

| Core panel | Allowed in |
| --- | --- |
| `core.search`, `core.engine_status`, `core.mode_switch`, `core.drawer_toggle` | toolbar |
| `core.clear_controls` | toolbar, playback |
| `core.libraries`, `core.playlists` | sidebar |
| `core.document_view` | main |
| `core.slide_navigator`, `core.slide_editor`, `core.inspector` | edit |
| `core.output_previews` | playback |
| `core.live_layers` | playback, sidebar |

- A panel in a pane it doesn't allow is moved to its default placement. A panel whose plugin isn't installed is skipped and kept in the file.
- Layouts come from the app (`core.default`), from the operator (`user`), or from layout plugins (provider = plugin id). `tests/validate.py` contains a reference implementation of the load check.

### Native modules

- A dylib exporting `em_module_entry`. The engine refuses a module whose `abi_version` differs from `EM_ABI_VERSION`.
- C ABI only. Structs that can grow carry `struct_size`; fields are only appended; enums are only extended.
- **The output's clock drives the engine.** `frame_needed` is the output's tick, so a genlocked SDI output paces rendering to house sync.
- **Zero copy.** The engine allocates output buffers (GPU-shared on Apple silicon) to the module's `em_buffer_requirements`. The module reads a scheduled buffer until it reports `frame_completed` and never writes to it. Input frames are module-owned and returned through `release`.
- Callbacks never block. Provider functions are never called concurrently on the same handle.
- Modules load at runtime as separate binaries, which keeps proprietary SDKs (DeckLink, NDI) out of the MPL-2.0 core.

---

## 5. Verification status

Run everything with `tests/run_all.sh` (needs python3, clang, and `pip install grpcio-tools jsonschema slangpy`).

| Item | Checked how |
| --- | --- |
| Protobuf schemas (engine and library) | Compile with `protoc` (Python output plus a full descriptor set); a test builds and round-trips a `ClientMessage` and an `EngineMessage` |
| Module ABI | `null_output.c` and `tests/module_host_smoke.c` build with `-Wall -Wextra -Werror`; the smoke test loads the module with `dlopen`, prerolls, plays 0.5 s at 59.94 and checks there are no drops; the header also compiles as C++20 |
| JSON schemas | Every example manifest and UI file validates; deliberately broken examples are rejected |
| First-party importers (PresentationMacUI) | `importers/core.office-import/tests/test_office_import.py`: Snappy vectors, a synthetic `.pages` with real IWA framing, generated `.docx` files (soft breaks, colors, strike, highlight, images, tables, encrypted and corrupt files), folder packages and Pages '09 files. With `PP7GEN` set: all 24 pp7gen `.docx`/`.pages` files import end to end, and `verify_against_pp7gen.py` checks formatting fidelity against pp7gen's `expected.json` (12/12 Pages fixtures and 5/8 Word fixtures exact; the remaining Word differences are pp7gen's deliberate slide-level restyling) |
| Import adapters | `tests/test_import.py` starts the reference .docx and .txt adapters as the UI would and checks them over the real framing: song-set splitting by localized Heading 1, labels from headings and label lines, stanza grouping, bold kept in RTF, CCLI and © to metadata, soft-break and page-break handling, lines per slide, title slides, Unicode escaping, and error codes for corrupt and binary files. The importer-only manifest and both option forms validate |
| 3D projection | `tests/reference/projection.py` checks z = 0 is unchanged, positive z enlarges, rotations recede the correct edges, orthographic, culling and stable depth ordering, and that `projection_cases.json` matches |
| Slang shaders | Every example pass compiles and links against `engine_effect.slang` on Slang's CPU target; reflection shows `params` and `tex_<id>` under the manifest's names; a shader using a missing interface field is rejected. Metal codegen is checked by the macOS build, not here |
