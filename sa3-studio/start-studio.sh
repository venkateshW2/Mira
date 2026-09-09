#!/usr/bin/env bash
# start-studio.sh — one-command launcher for the SA3 LoRA studio.
# Place this INSIDE ~/sa3-studio/ (next to env.sh) and run:  ./start-studio.sh
set -euo pipefail

# --- locate the folder (works even if you rename/move it) ---
STUDIO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export STUDIO

# --- load the redirected caches (HF_HOME, UV_CACHE_DIR, etc.) ---
if [ -f "$STUDIO/env.sh" ]; then
  # shellcheck disable=SC1091
  source "$STUDIO/env.sh"
else
  echo "!! env.sh not found in $STUDIO — did you run Step 1 of SETUP.md?"; exit 1
fi

# --- preflight: make sure the pieces exist before launching ---
missing=0
for d in "$STUDIO/underfit" "$STUDIO/stable-audio-3/optimized/mlx"; do
  if [ ! -d "$d" ]; then echo "!! missing: $d"; missing=1; fi
done
if [ "$missing" -eq 1 ]; then
  echo "Setup incomplete. Run the remaining steps in SETUP.md, then retry."; exit 1
fi

echo ">> STUDIO   = $STUDIO"
echo ">> HF_HOME  = ${HF_HOME:-unset}"
echo ">> engine   = MLX (Apple Silicon / Metal)"
echo ">> model    = use sa3-sm-music on this 16GB M1 Pro (medium = Studio only)"
echo ">> dashboard: http://localhost:8787   (leave this terminal open)"
echo

# --- launch the dashboard on the MLX engine ---
cd "$STUDIO/underfit"
exec env \
  UNDERFIT_ENGINE=mlx \
  UNDERFIT_MLX_ROOT="$STUDIO/stable-audio-3/optimized/mlx" \
  ./run.sh
