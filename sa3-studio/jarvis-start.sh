#!/usr/bin/env bash
# jarvis-start.sh — start the underfit dashboard on a JarvisLabs instance.
# Run this every time you `jl resume`.
set -euo pipefail
WS=/home/workspace
export HF_HOME="$WS/.hf"
export UNDERFIT_STATE_DIR="$WS/underfit/state"
export UNDERFIT_MODELS_DIR="$WS/underfit/state/models"
export PATH="$HOME/.local/bin:$PATH"

# ffmpeg is the one dependency that lives outside /home, so a pause eats it.
# It is OPTIONAL — without it the dashboard skips ground-truth previews and demos
# fall back to WAV instead of MP3 — so never let its absence stop the run.
if ! command -v ffmpeg >/dev/null 2>&1; then
  echo "== ffmpeg missing after resume, reinstalling (optional, ~10 s) =="
  (apt-get update -qq && apt-get install -y -qq ffmpeg) >/dev/null 2>&1 \
    || echo "   ffmpeg install failed — continuing, demos will be WAV"
fi

pkill -f 'dashboard/server.py' 2>/dev/null || true
sleep 2

# .venv/bin/python, NOT `uv run` — uv run re-syncs and downgrades safetensors.
cd "$WS/underfit"
nohup .venv/bin/python dashboard/server.py > "$WS/dashboard.log" 2>&1 &
sleep 15

echo "dashboard on port 8787 — open the HTTP endpoint from \`jl show <id>\`"
echo "(the instance must have been created with --http-ports 8787)"
tail -3 "$WS/dashboard.log"
