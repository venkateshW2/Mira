#!/usr/bin/env bash
# generate.sh — one command to open the SA3 generate UI with your LoRAs loaded.
#
#   ./generate.sh                    launch (or focus an already-running UI)
#   ./generate.sh ~/Downloads/foo    also register checkpoints from that file/folder
#   ./generate.sh stop               shut it down
#
# Checkpoints are symlinked into the UI's loras/sa3-medium/ so they appear in the
# in-page dropdown — you can load up to 3 slots and A/B them without relaunching.
set -euo pipefail

STUDIO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MLX="$STUDIO/stable-audio-3/optimized/mlx"
PY="$MLX/.venv/bin/python"
PORT=7860
URL="http://127.0.0.1:$PORT"
LORA_DIR="$MLX/loras/sa3-medium"
LOG="$STUDIO/.generate.log"

# Where to look for checkpoints, in addition to any path passed as $1.
SEARCH_DIRS=("$HOME/Downloads" "$STUDIO/loras")

if [ "${1:-}" = "stop" ]; then
  pids=$(lsof -ti:"$PORT" 2>/dev/null || true)
  [ -n "$pids" ] && kill $pids && echo "stopped." || echo "nothing running on $PORT."
  exit 0
fi

[ -x "$PY" ] || { echo "!! no venv at $PY — see SETUP.md"; exit 1; }
mkdir -p "$LORA_DIR"

# --- register checkpoints (symlink, so nothing is copied or moved) -------------
link_ckpt() {
  local src="$1" name
  name="$(basename "$src")"
  # '=' in underfit's filenames is legal but awkward on a command line; normalise.
  name="${name//=/}"
  ln -sfn "$src" "$LORA_DIR/$name"
}

found=0
for d in "${SEARCH_DIRS[@]}" ${1:+"$1"}; do
  [ -e "$d" ] || continue
  if [ -f "$d" ]; then link_ckpt "$d"; found=$((found+1)); continue; fi
  while IFS= read -r f; do link_ckpt "$f"; found=$((found+1)); done \
    < <(find "$d" -maxdepth 2 -name "*.safetensors" -size +1M 2>/dev/null)
done

# Drop symlinks whose target has gone away, so the dropdown stays honest.
find "$LORA_DIR" -type l ! -exec test -e {} \; -delete 2>/dev/null || true

echo ">> LoRAs available in the dropdown:"
ls -1 "$LORA_DIR" 2>/dev/null | sed 's/^/     /' || echo "     (none found)"
[ "$found" -eq 0 ] && echo "     no .safetensors found — pass one: ./generate.sh /path/to/ckpt"

# --- already up? just focus it -------------------------------------------------
if lsof -ti:"$PORT" >/dev/null 2>&1; then
  echo ">> already running — opening $URL"
  open "$URL"; exit 0
fi

# --- deps the UI needs on top of the inference venv ----------------------------
if ! "$PY" -c "import gradio, PIL" 2>/dev/null; then
  echo ">> installing UI deps (gradio, pillow) into the MLX venv..."
  UV_CACHE_DIR="$STUDIO/.uvcache" uv pip install --quiet --python "$PY" gradio pillow
fi

# --- launch --------------------------------------------------------------------
# --decoder same-l is NOT optional: it must match the encoder the latents were
# made with, and omitting it drops the script into an interactive picker.
echo ">> starting SA3 medium (same-l) on $URL — first generate loads the DiT (~45s)"
cd "$MLX"
nohup "$PY" scripts/sa3_gradio.py \
  --dit medium --decoder same-l \
  --default-seconds 120 --default-steps 8 \
  --no-share --port "$PORT" > "$LOG" 2>&1 &

for _ in $(seq 1 60); do
  if curl -fs -o /dev/null "$URL" 2>/dev/null; then
    open "$URL"; echo ">> ready. log: $LOG   stop with: ./generate.sh stop"; exit 0
  fi
  sleep 1
done
echo "!! did not come up in 60s — check $LOG"; tail -20 "$LOG"; exit 1
