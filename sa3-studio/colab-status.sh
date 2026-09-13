#!/usr/bin/env bash
# colab-status.sh — what's happening on Colab right now.
# Usage:  ./colab-status.sh [session-name]     (default: sa3)
S="${1:-sa3}"

echo "══ SESSIONS (is anything costing money?) ══"
colab sessions

echo
echo "══ HARDWARE ══"
colab status -s "$S" 2>/dev/null || { echo "  no session '$S'"; exit 0; }

echo
echo "══ GPU + TRAINING ══"
cat <<'PY' | colab exec -s "$S" 2>/dev/null | grep -v '^\s*$'
import subprocess
def sh(c): return subprocess.run(c, shell=True, capture_output=True, text=True).stdout.strip()
print("GPU:", sh("nvidia-smi --query-gpu=utilization.gpu,memory.used,memory.total --format=csv,noheader") or "n/a")
print("Disk:", sh("df -h /content | tail -1 | awk '{print $3\" used, \"$4\" free\"}'"))
print("Models:", sh("du -sh /root/.cache/huggingface 2>/dev/null | cut -f1") or "none")
print("Latents:", sh("ls /content/latents/*/*.npy 2>/dev/null | wc -l").strip(), "files")
util = sh("nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader,nounits") or "0"
busy = sh("ps -eo args | grep -c '[l]ora_train_mlx\\|[t]rain_lora\\|[l]ora_train.py'") or "0"
print("Training running:", "YES" if busy.strip() not in ("0","") else "no",
      f"(GPU {util}% busy)")
log = sh("ls -t /content/underfit/state/runs/*.log 2>/dev/null | head -1")
if log:
    print("\n--- last 3 lines of", log.split('/')[-1], "---")
    print(sh(f"tail -3 '{log}'"))
ck = sh("ls -1t /content/underfit/state/runs/*/*/checkpoints/*.safetensors 2>/dev/null | head -3")
print("\n--- newest checkpoints ---")
print(ck if ck else "  none yet")
PY

echo
echo "  watch live:   colab console -s $S"
echo "  stop paying:  colab stop -s $S"
