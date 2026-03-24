#!/usr/bin/env bash
set -euo pipefail

FILES=(
  "FINAL_VALIDATION_ocx_type2_teensy.md"
  "README.md"
  "ocx_type2_harness.py"
  "tests/test_ocx_type2.py"
)

echo "[1/4] Resolving target conflict files with current branch versions (--ours)..."
git checkout --ours "${FILES[@]}"
git add "${FILES[@]}"

echo "[2/4] Verifying no conflict markers..."
rg '^(<<<<<<<|=======|>>>>>>>)' . && {
  echo "Conflict markers still present." >&2
  exit 1
} || true

echo "[3/4] Running mandatory checks..."
git diff --check
python -m py_compile ocx_type2_wav_sim.py
python -m py_compile ocx_type2_harness.py
python -m py_compile tests/test_ocx_type2.py
pytest -q
pio run -e teensy41

echo "[4/4] Checks green. Finalize merge/rebase manually:"
echo "  git commit -m 'Resolve merge conflicts for docs/harness/tests'"
