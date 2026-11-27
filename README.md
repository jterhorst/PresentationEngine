# PresentationEngine

An open-source, high-performance presentation engine in the spirit of ProPresenter: a headless-capable Metal renderer that runs as its own process, drives screen, SDI (Blackmagic DeckLink) and NDI outputs, and is controlled by separate UI clients over a versioned protocol.

The macOS app lives in [PresentationMacUI](https://github.com/jterhorst/PresentationMacUI). This repository holds the engine and the **contracts** every client and plugin builds against.

> **Status:** design and contracts. The engine itself (C++20, Metal) starts with Phase 0 of the [roadmap](docs/architecture.md#roadmap).

## Principles

- **The frame is sacred.** The render thread never waits on disk, network, IPC, plugins or the UI.
- **The core stays small.** If a feature can be a plugin or module, it is one.
- **Never regress rendering performance.** CI gates every merge on deterministic counters and timed reference scenes.

## Layout

| Path | What |
| --- | --- |
| [`docs/architecture.md`](docs/architecture.md) | The architecture plan: processes, render pipeline, video I/O, protocol, plugins, UI, import, testing, roadmap, decisions |
| [`CONTRACTS.md`](CONTRACTS.md) | How the contracts fit together: framing, versioning, roles, state sync, data model and extensions, bindings, plugins, import adapters, layouts |
| `proto/engine/v1/` | Engine ↔ client protocol (Protobuf) |
| `proto/library/v1/` | Library, document and playlist file format, shared by the native and web UIs |
| `proto/importer/v1/` | Import adapter protocol |
| `include/engine/module.h` | Native module C ABI (outputs, inputs) |
| `schemas/` | JSON Schemas: GPU packages, logic plugins, declarative plugin UI, workspace layouts |
| `shaders/engine_effect.slang` | Shader interface for effects, transitions and sources |
| `examples/` | Null output module, GPU package, timers logic plugin, default layout, example import adapters |
| `tests/` | Contract checks, including the 3D projection reference implementation |

## Running the contract checks

Requires Python 3, clang (or cc), and:

```sh
pip install grpcio-tools jsonschema slangpy
tests/run_all.sh
```

## License

[MPL-2.0](LICENSE). Proprietary SDKs (DeckLink, NDI) are used only from separately distributed modules.
