#!/usr/bin/env bash
# runpod-setup.sh — one-time SA3 + underfit install onto a RunPod NETWORK VOLUME.
#
#   curl -sL https://raw.githubusercontent.com/venkateshW2/Mira/main/sa3-studio/runpod-setup.sh | bash
#
# Everything lands under /workspace (the network volume), so it SURVIVES the pod
# being stopped. That is the whole point versus Colab: you never re-download the
# 24 GB of weights again.
set -euo pipefail

WS=/workspace
export HF_HOME="$WS/.hf"
export UV_CACHE_DIR="$WS/.uvcache"
mkdir -p "$WS" "$HF_HOME" "$UV_CACHE_DIR" "$WS/datasets" "$WS/loras"
cd "$WS"

echo "== GPU =="
nvidia-smi --query-gpu=name,memory.total --format=csv,noheader

echo "== uv =="
command -v uv >/dev/null || curl -LsSf https://astral.sh/uv/install.sh | sh
export PATH="$HOME/.local/bin:$PATH"

echo "== repos =="
[ -d "$WS/underfit" ]        || git clone --depth 1 https://github.com/dada-bots/underfit        "$WS/underfit"
[ -d "$WS/stable-audio-3" ]  || git clone --depth 1 https://github.com/Stability-AI/stable-audio-3 "$WS/stable-audio-3"

echo "== underfit deps (~5 GB) =="
cd "$WS/underfit"
./install.sh --no-setup

echo "== safetensors pin =="
# uv run re-syncs the venv to underfit's lockfile and downgrades safetensors to
# 0.7.0, which breaks transformers at model build. Pin it AFTER install, and
# never launch the dashboard with `uv run` (see start.sh).
uv pip install --python "$WS/underfit/.venv/bin/python" 'safetensors>=0.8.0'
"$WS/underfit/.venv/bin/python" -c "import safetensors; print('safetensors', safetensors.__version__)"

echo "== SA3 backend + sa3-medium weights (~24 GB) =="
if [ -z "${HF_TOKEN:-}" ]; then
  echo "!! HF_TOKEN not set. export HF_TOKEN=hf_... and re-run."
  echo "   Also click 'Agree and access repository' once at:"
  echo "   https://huggingface.co/stabilityai/stable-audio-3-medium"
  exit 1
fi
cd "$WS/underfit"
uv run python -m underfit.cli.setup \
    --backend sa3 --backend-path "$WS/stable-audio-3" --models sa3-medium

echo
echo "DONE. Weights + venvs live on the network volume."
echo "Start the dashboard with:  bash $WS/start.sh"
