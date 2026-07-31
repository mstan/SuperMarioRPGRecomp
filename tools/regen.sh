#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

PYTHON="${PYTHON:-$(command -v python3 || command -v python || true)}"
SNESRECOMP_ROOT="${SNESRECOMP_ROOT:-F:/Projects/snesrecomp/_wt_smrpg_sa1_snesrecomp}"
EXPECTED_SHA256="740646f3535bfb365ca44e70d46ab433467b142bd84010393070bd0b141af853"

if [ -z "$PYTHON" ] || [ ! -f "$SNESRECOMP_ROOT/tools/v2_emit.py" ]; then
  echo "regen.sh: Python or SNESRECOMP_ROOT is unavailable" >&2
  exit 1
fi
if [ ! -f smrpg.sfc ]; then
  echo "regen.sh: stage the verified US ROM as smrpg.sfc" >&2
  exit 1
fi

ACTUAL_SHA256="$("$PYTHON" - <<'PY'
from pathlib import Path
import hashlib
print(hashlib.sha256(Path("smrpg.sfc").read_bytes()).hexdigest())
PY
)"
if [ "$ACTUAL_SHA256" != "$EXPECTED_SHA256" ]; then
  echo "regen.sh: smrpg.sfc SHA-256 mismatch" >&2
  exit 1
fi

"$PYTHON" "$SNESRECOMP_ROOT/tools/build_native_analyzer.py"
"$PYTHON" "$SNESRECOMP_ROOT/tools/v2_emit.py" --rom smrpg.sfc \
  --cfg-dir recomp --out-dir src/gen --cfg-roots --no-host-root-scan \
  --analysis-backend native
"$PYTHON" "$SNESRECOMP_ROOT/tools/v2_sync_funcs_h.py" \
  --cfg-dir recomp --out recomp/funcs.h
