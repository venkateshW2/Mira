#!/usr/bin/env bash
# check-studio.sh — preflight / health report for the SA3 LoRA studio.
# Run anytime:  ./check-studio.sh   (read-only; changes nothing)
STUDIO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
echo "=== SA3 STUDIO HEALTH CHECK ==="
echo "folder: $STUDIO"
echo

echo "--- free disk (internal) ---"
df -h / | awk 'NR==1 || NR==2 {print $4" free of "$2}'
echo

echo "--- folder size (how big the studio has grown) ---"
du -sh "$STUDIO" 2>/dev/null
echo

echo "--- required pieces present? ---"
for d in env.sh underfit stable-audio-3 stable-audio-3/optimized/mlx underfit/state .hf .uvcache; do
  if [ -e "$STUDIO/$d" ]; then echo "  ok   $d"; else echo "  MISS $d"; fi
done
echo

echo "--- downloaded model packs (HF cache inside folder) ---"
if [ -d "$STUDIO/.hf/hub" ]; then
  ls -1 "$STUDIO/.hf/hub" 2>/dev/null | grep -i "stable-audio" || echo "  (none yet — will download on first dashboard use)"
  du -sh "$STUDIO/.hf" 2>/dev/null | awk '{print "  HF cache size: "$1}'
else
  echo "  (no HF cache yet)"
fi
echo

echo "--- trained LoRAs so far ---"
find "$STUDIO" -name "*.safetensors" -type f 2>/dev/null -exec du -h {} + 2>/dev/null | sort -rh | head -10
[ -z "$(find "$STUDIO" -name '*.safetensors' -type f 2>/dev/null)" ] && echo "  (none yet)"
echo

echo "--- toolchain ---"
for t in git curl uv ffmpeg; do
  printf "  %-6s " "$t"; command -v "$t" >/dev/null 2>&1 && echo "ok" || echo "MISSING"
done
echo
echo "Done. To launch:  ./start-studio.sh"
