"""Compile and link every example GPU-package shader against engine_effect.slang.

Uses slangpy's CPU device so it runs on any CI machine (no GPU). It checks Slang
syntax, types and the engine interface, and prints reflected parameter names
(the engine binds params and textures by these names). Metal-specific codegen
is covered by the macOS build.

Usage: pip install slangpy jsonschema && python3 tests/compile_shaders.py
"""
import json
import pathlib
import sys

import slangpy as spy

ROOT = pathlib.Path(__file__).resolve().parent.parent
failures = 0

for pkg_dir in sorted((ROOT / "examples/gpu-package").iterdir()):
    manifest = json.loads((pkg_dir / "package.json").read_text())
    dev = spy.Device(type=spy.DeviceType.cpu,
                     compiler_options={"include_paths": [str(ROOT / "shaders"), str(pkg_dir / "shaders")]})
    items = manifest.get("effects", []) + manifest.get("transitions", []) + manifest.get("sources", [])
    for item in items:
        passes = item.get("passes") or [dict(item["shader"], id="main")]
        for p in passes:
            module_name = pathlib.Path(p["file"]).stem
            try:
                module = dev.load_module(module_name)
                prog = dev.link_program(modules=[module], entry_points=[module.entry_point(p["entry"])])
                names = {param.name for param in prog.layout.parameters}
                # Params and declared textures must be reflected under the manifest's names.
                missing = []
                if item.get("params") and "params" not in names:
                    missing.append("params")
                missing += [f"tex_{t}" for t in item.get("textures", []) if f"tex_{t}" not in names]
                if missing:
                    raise RuntimeError(f"missing reflected bindings: {missing}")
                print(f"PASS {manifest['id']}/{item['id']} pass {p['id']} ({p['entry']})")
            except Exception as e:  # noqa: BLE001
                failures += 1
                print(f"FAIL {manifest['id']}/{item['id']} pass {p['id']}: {str(e)[:500]}")

print("ALL PASSED" if failures == 0 else f"{failures} FAILED")
sys.exit(1 if failures else 0)
