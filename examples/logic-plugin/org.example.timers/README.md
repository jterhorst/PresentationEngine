# Timers: an example logic plugin

This folder shows the contract only; it has no executable.

## State it publishes (`PluginState.snapshot_json`)

```json
{
  "timers": [
    {
      "id": "t1",
      "name": "Sermon",
      "duration_ms": 1800000,
      "visible": true,
      "running": true,
      "stopped": false,
      "countdown": { "target_ns": 5400000000000, "running": true, "paused_remaining_ns": 0, "allow_negative": true }
    }
  ]
}
```

The UI's `readout` with `format: "countdown"` animates `countdown` locally from Heartbeat engine time, so no state traffic happens every second.

## Session flow

1. The plugin host starts `bin/timers` with `ENGINE_PLUGIN_SOCKET` set.
2. The plugin sends `Hello` (kind `CLIENT_KIND_PLUGIN_HOST`, manifest JSON) and receives `Welcome` with role `ROLE_PLUGIN` and its granted capabilities.
3. `PluginReady`, then `PluginSubscribe { events: [EVENT_TYPE_PLUGIN_TRIGGER, EVENT_TYPE_DOCUMENT_LOADED] }`.
4. `PluginUi` with `ui.json`, and `PluginState` with a snapshot.
5. When the operator clicks **Start** on timer `t1`, the engine forwards `PluginActionInvoked { action: "start", payload_json: "{\"id\":\"t1\"}" }`.
6. The plugin sends, in order:
   - `BindingValues { values: { "t1": Countdown { target_ns: now + remaining, running: true } } }`
   - `SceneEditBatch [ show_fixture: FixtureDef { id: "plugin:org.example.timers/t1", z: 150, elements: [ TextElement "{plugin:org.example.timers/t1|mm:ss}" ] } ]` when the timer is visible.
   - `PluginState` as a JSON Patch: `[{"op":"replace","path":"/timers/0/running","value":true}, ...]`.
7. The engine renders the digits every frame from the binding. The plugin sends nothing more until the next operator action.

## Persisting its own data

The plugin stores data in the document in two ways, without the core model knowing anything about timers.

**Slide extension.** The `slide_timer` inspector edits `Slide.extensions["org.example.timers"]`:

```json
{ "schema_version": 1, "data": { "auto_start": true, "timer_id": "t1", "show_fixture": true } }
```

The UI writes this into its document and saves it. When that slide is taken, the plugin's `SLIDE_TAKEN` event includes the slide with its extensions, and the plugin starts `t1`. The state includes `"timer_options": [{"value": "t1", "label": "Sermon"}]` for the inspector's picker.

**Element type.** `timer_display` is a `PluginElement`:

```json
{ "plugin_id": "org.example.timers", "type": "timer_display",
  "content": { "schema_version": 1, "data": { "timer_id": "t1", "format": "mm:ss" } },
  "fallback": [ { "id": "f1", "text": { "runs": [ { "text": "30:00" } ] } } ] }
```

After a document loads, the engine sends `ResolvePluginElements`. The plugin answers with a text element containing `{plugin:org.example.timers/t1|mm:ss}`. The engine renders that, and the UI saves it as the new fallback. If the plugin isn't installed, the fallback renders and the extension data stays in the file untouched.

## Slide-triggered actions

A slide can also start a timer without the operator: a `SlideAction.plugin_trigger { plugin_id: "org.example.timers", action: "start", payload_json: "{\"id\":\"t1\"}" }` arrives as an `EVENT_TYPE_PLUGIN_TRIGGER` event.
