#!/usr/bin/env bash
# Runs every contract check. Requirements: python3, clang (or cc), and
#   pip install grpcio-tools jsonschema slangpy
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="${TMPDIR:-/tmp}/engine-contracts"
rm -rf "$OUT" && mkdir -p "$OUT/py"

echo "== Protobuf: compile and round-trip"
python3 -m grpc_tools.protoc -I proto --python_out="$OUT/py" proto/engine/v1/*.proto proto/library/v1/*.proto proto/importer/v1/*.proto
python3 tests/validate.py "$OUT/py"

echo "== Import adapters: reference adapters end to end"
python3 tests/test_import.py "$OUT/py"

echo "== 3D projection: reference implementation and fixture"
python3 tests/reference/projection.py

echo "== Slang: compile and link example shaders"
python3 tests/compile_shaders.py

echo "== Module ABI: build example module and smoke host"
CC="${CC:-$(command -v clang || command -v cc)}"
EXT=so; [[ "$(uname)" == "Darwin" ]] && EXT=dylib
"$CC" -std=c11 -Wall -Wextra -Werror -O2 -shared -fPIC -fvisibility=hidden -Iinclude \
  examples/modules/null_output.c -o "$OUT/null_output.$EXT" -lpthread
"$CC" -std=c11 -Wall -Wextra -Werror -O2 -Iinclude tests/module_host_smoke.c -o "$OUT/smoke" \
  $([[ "$(uname)" == "Darwin" ]] || echo -ldl) -lpthread
"$OUT/smoke" "$OUT/null_output.$EXT"

echo "== All contract checks passed"
