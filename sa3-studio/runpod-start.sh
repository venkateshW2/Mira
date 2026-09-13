#!/usr/bin/env bash
# runpod-start.sh — start the underfit dashboard on a RunPod pod.
# Run this every time you start a pod that has the network volume attached.
set -euo pipefail
WS=/workspace
export HF_HOME="$WS/.hf"
export UNDERFIT_STATE_DIR="$WS/underfit/state"
export UNDERFIT_MODELS_DIR="$WS/underfit/state/models"

pkill -f 'dashboard/server.py' 2>/dev/null || true
sleep 2

# .venv/bin/python, NOT `uv run` — uv run re-syncs and downgrades safetensors.
cd "$WS/underfit"
nohup .venv/bin/python dashboard/server.py > "$WS/dashboard.log" 2>&1 &
sleep 15
echo "dashboard on port 8787 — expose it in the RunPod pod's HTTP ports"
tail -3 "$WS/dashboard.log"
