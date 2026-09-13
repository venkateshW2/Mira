#!/usr/bin/env bash
# prepare-lora.sh — one film folder -> a zip ready to upload to RunPod/Colab.
#
#   ./prepare-lora.sh "/Volumes/T7 Shield 1/TO-TRAIN/MadMAx" xyr
#   ./prepare-lora.sh <folder> <trigger> [max_duration]
#
# Three steps, in the order they MUST happen:
#   1. captions  -> mira writes a <stem>.json SA3 sidecar beside each audio file.
#   2. encode    -> pre_encode reads those sidecars (pre_encode_mlx.py:144) and writes
#                   <stem>.npy latents + a <stem>.json carrying the tags forward.
#   3. zip       -> the latents dir, ready to upload.
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

FOLDER="${1:-}"; TRIGGER="${2:-}"; MAXDUR="${3:-600}"
if [ -z "$FOLDER" ] || [ -z "$TRIGGER" ]; then
  echo "usage: $0 <audio-folder> <trigger-token> [max_duration_seconds]"; exit 1
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

echo "── 3. verify the tags actually survived ─────────────────────────"
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
echo
echo "Upload that zip, and train with --trigger $TRIGGER"
