#!/usr/bin/env bash
# prepare-lora.sh — one film folder -> a zip ready to upload to JarvisLabs.
#
#   ./prepare-lora.sh "/Volumes/T7 Shield 1/TO-TRAIN/MadMAx" xyr
#   ./prepare-lora.sh <folder> <trigger> [max_duration] [--push root@<ip>]
#
# Five steps, in the order they MUST happen:
#   1. captions  -> mira writes a <stem>.json SA3 sidecar beside each audio file.
#   2. encode    -> pre_encode reads those sidecars (pre_encode_mlx.py:144) and writes
#                   <stem>.npy latents + a <stem>.json carrying the tags forward.
#   3. stand-ins -> 8 KB silent .wav beside each latent. NOT optional: underfit finds
#                   tags by walking the folder for AUDIO and using .json only as a
#                   stem-keyed lookup against what it found, so a latents-only folder
#                   renders no tag pills and posts tag_keys: [] -- every caption
#                   discarded, silently. See TRAINING.md section 3.
#   5. zip       -> the latents dir, ready to upload by hand.
#   5. --push    -> optional: rsync straight to a GPU box and register the dataset.
#                   On a re-tag only the .json move; rsync skips the unchanged .npy.
#
# Step 1 is not optional and not reorderable: pre_encode returns {} for a file with no
# sidecar beside it, and a dataset of empty tags trains a LoRA that only ever saw the
# trigger -- with no error anywhere. That failure has already happened once on this
# project (see COLAB-TRAINING.md).
set -euo pipefail

STUDIO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$STUDIO/.." && pwd)"
MIRA="$REPO/build/src/mira"
PY="$STUDIO/stable-audio-3/optimized/mlx/.venv/bin/python"
DB="${MIRA_DB:-$HOME/.mira/library.db}"

PUSH=""; SSH_KEY="${SSH_KEY:-$HOME/.ssh/id_ed25519}"
ARGS=()
while [ $# -gt 0 ]; do
  case "$1" in
    --push) PUSH="${2:-}"; shift 2 ;;
    *) ARGS+=("$1"); shift ;;
  esac
done
set -- ${ARGS[@]+"${ARGS[@]}"}

FOLDER="${1:-}"; TRIGGER="${2:-}"; MAXDUR="${3:-600}"
if [ -z "$FOLDER" ] || [ -z "$TRIGGER" ]; then
  echo "usage: $0 <audio-folder> <trigger> [max_duration] [--push root@<ip>]"; exit 1
fi
[ -d "$FOLDER" ] || { echo "!! not a folder: $FOLDER"; exit 1; }
[ -x "$MIRA" ]   || { echo "!! mira CLI not built: $MIRA"; exit 1; }
[ -x "$PY" ]     || { echo "!! MLX venv missing: $PY"; exit 1; }

NAME="$(basename "$FOLDER" | tr ' ' '-' | tr -cd '[:alnum:]-_')"
OUT="$STUDIO/latents/$NAME"
ZIP="$STUDIO/$NAME-latents.zip"

echo "── 1. captions (trigger: $TRIGGER) ──────────────────────────────"
# Driven from the DB, not the filesystem: only analysed files have anything to caption,
# and this way the count below is a real check rather than a guess.
COUNT=0; MISSING=0
while IFS= read -r f; do
  if [ ! -f "$f" ]; then MISSING=$((MISSING+1)); continue; fi
  "$MIRA" caption "$f" --trigger "$TRIGGER" --emit-sidecar --db "$DB" >/dev/null 2>&1 \
    && COUNT=$((COUNT+1)) || echo "   caption failed: $(basename "$f")"
done < <(sqlite3 "$DB" "SELECT path FROM files
                        WHERE path LIKE '$(echo "$FOLDER" | sed "s/'/''/g")%'
                          AND analyzed_at IS NOT NULL;")
echo "   wrote $COUNT sidecar(s)${MISSING:+, $MISSING file(s) missing on disk}"
[ "$COUNT" -eq 0 ] && { echo "!! no sidecars written — is this folder scanned AND analysed?"; exit 1; }

echo "── 2. pre-encode (max_duration ${MAXDUR}s) ──────────────────────"
mkdir -p "$OUT"
printf '%s\n' \
  "{\"id\":1,\"cmd\":\"pre_encode\",\"audio_dir\":$(python3 -c 'import json,sys;print(json.dumps(sys.argv[1]))' "$FOLDER"),\"output_dir\":$(python3 -c 'import json,sys;print(json.dumps(sys.argv[1]))' "$OUT"),\"max_duration\":$MAXDUR}" \
  '{"id":2,"cmd":"quit"}' \
| "$PY" "$STUDIO/sa3_worker.py" --dit medium --decoder same-l 2>&1 | grep -vE '^\{"id": null'

echo "-- 3. audio stand-ins --------------------------------------------"
# 8 KB of silence per latent, named to match. underfit needs a recognised audio extension
# and >= 4096 bytes; it never reads these for training, which reads latents. Real audio
# works too and buys ground-truth previews, but costs ~16x the upload for a file the
# trainer never opens -- 4.6 GB went up once before that was understood.
python3 - "$OUT" <<'STANDIN'
import sys, wave
from pathlib import Path
SIL = b"\x00\x00" * 4096
out = Path(sys.argv[1]); n = 0
for npy in sorted(out.glob("*.npy")):
    wav = npy.with_suffix(".wav")
    if wav.exists():
        continue
    with wave.open(str(wav), "wb") as o:
        o.setnchannels(1); o.setsampwidth(2); o.setframerate(44100)
        o.writeframes(SIL)
    n += 1
print(f"   {n} stand-in(s) written ({len(list(out.glob('*.wav')))} total, ~8 KB each)")
STANDIN

echo "── 4. verify the tags actually survived ─────────────────────────"
# The check that matters. A latents .json with only path/seconds_total and no tag keys
# means step 1 did not reach this file, and training would silently collapse to the
# trigger alone.
"$PY" - "$OUT" "$TRIGGER" <<'PY'
import json, sys, pathlib
out, trigger = pathlib.Path(sys.argv[1]), sys.argv[2]
js = [p for p in out.glob("*.json") if p.name != "details.json"]
tagged = [p for p in js if json.loads(p.read_text()).get("trigger") == trigger]
print(f"   {len(js)} latent sidecar(s), {len(tagged)} carrying trigger '{trigger}'")
if js and not tagged:
    sys.exit("!! NO tags made it through — captions did not land beside the audio")
if len(tagged) < len(js):
    print(f"   WARNING: {len(js)-len(tagged)} file(s) have no tags and will train on the trigger alone")
sample = json.loads(tagged[0].read_text()) if tagged else {}
for k in ("trigger", "rhythm", "dynamics", "texture", "moods", "instruments"):
    if k in sample: print(f"     {k}: {sample[k]}")
PY

echo "── 4. zip ───────────────────────────────────────────────────────"
rm -f "$ZIP"
(cd "$OUT" && zip -qr "$ZIP" .)
echo "   $ZIP  ($(du -h "$ZIP" | cut -f1))"

if [ -n "$PUSH" ]; then
  echo "-- 6. push to $PUSH ----------------------------------------------"
  REMOTE_DIR="/home/workspace/datasets/$NAME"
  # -F /dev/null bypasses a local ~/.ssh/config RemoteCommand, which otherwise makes every
  # non-interactive ssh fail with "Cannot execute command-line and remote command."
  SSH="ssh -o StrictHostKeyChecking=no -F /dev/null -i $SSH_KEY"
  rsync -a --info=stats1 -e "$SSH" "$OUT/" "$PUSH:$REMOTE_DIR/"

  # Registering is a separate act from copying, and the cache clear is not optional:
  # underfit reads the folder ONCE at import and caches the scan in <ds_id>_tags.json.
  # Re-importing after a re-tag without clearing it serves the old tags forever.
  $SSH "$PUSH" "bash -s" <<REMOTE
set -e
rm -f /home/workspace/underfit/state/datasets/*_tags.json
curl -s -X POST http://127.0.0.1:8787/api/datasets/import \
  -H 'Content-Type: application/json' \
  -d '{"path":"$REMOTE_DIR","name":"$NAME-$TRIGGER","mode":"preencoded_import","model":"sa3-medium"}'
echo
REMOTE
  echo "   registered as dataset '$NAME-$TRIGGER'"
fi

echo
echo "Train with --trigger $TRIGGER"
