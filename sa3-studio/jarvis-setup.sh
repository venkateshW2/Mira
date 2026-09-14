#!/usr/bin/env bash
# jarvis-setup.sh — one-time SA3 + underfit install onto a JarvisLabs instance.
#
#   curl -sL https://raw.githubusercontent.com/venkateshW2/Mira/main/sa3-studio/jarvis-setup.sh | bash
#
# Launch the instance first (India / Noida region, A30 24 GB):
#
#   jl create --gpu A30 --region IN2 --storage 50 --http-ports 8787 -n sa3
#
# 50 GB is the custom tier; ~32 GB is actually used (24 GB weights + ~5 GB deps
# + latent zips + checkpoints). Storage CANNOT BE REDUCED after creation and
# bills Rs 0.648/hr (~Rs 467/month) whether the instance runs or is paused, on
# top of the Rs 38.88/hr GPU. Ports 8889, 6006, 7007 and 22 are reserved by
# JarvisLabs; 8787 is free.
#
# 24 GB of VRAM is required, not optional: the config that works is BATCH 4, which
# lands near 15-17 GB. The "16 GB is enough, it peaked ~7 GB" note in TRAINING.md
# measured the batch-1 run, which came out undertrained. See TRAINING.md section 5.
#
# Everything lands under /home, which is the ONLY path JarvisLabs keeps across
# `jl pause` / `jl resume`. Anything installed elsewhere — including apt packages —
# is gone on resume. That is why the whole install goes into a uv venv under $WS.
set -euo pipefail

WS=/home/workspace
export HF_HOME="$WS/.hf"
export UV_CACHE_DIR="$WS/.uvcache"
mkdir -p "$WS" "$HF_HOME" "$UV_CACHE_DIR" "$WS/datasets" "$WS/loras"
cd "$WS"

echo "== GPU =="
nvidia-smi --query-gpu=name,memory.total --format=csv,noheader
# Warn early rather than OOM four hours into a run.
VRAM_MB=$(nvidia-smi --query-gpu=memory.total --format=csv,noheader,nounits | head -1)
if [ "$VRAM_MB" -lt 20000 ]; then
  echo "!! ${VRAM_MB} MiB of VRAM. Batch 4 needs ~24 GB. Destroy this and launch an A30/L4."
  exit 1
fi

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
# never launch the dashboard with `uv run` (see jarvis-start.sh).
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
echo "DONE. Weights + venv live under /home and survive \`jl pause\`."
echo "Storage bills while paused — \`jl destroy\` once the LoRAs are downloaded"
echo "if the next session is months away; re-running this costs ~30 min."
echo "Start the dashboard with:  bash $WS/jarvis-start.sh"
echo "Upload the latent zips to $WS/datasets/ and unzip them there."
