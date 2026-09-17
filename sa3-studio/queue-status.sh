#!/usr/bin/env bash
# queue-status.sh — what the two training boxes are doing, without disturbing them.
#
# Read-only: it asks the dashboard and reads the tail of the run log. Nothing here
# touches the GPU, the queue or the run state.
set -uo pipefail
K="$HOME/.ssh/id_ed25519"
# -F /dev/null is not decoration: a RemoteCommand in the local ~/.ssh/config makes every
# non-interactive ssh fail with "Cannot execute command-line and remote command."
SSH=(ssh -F /dev/null -i "$K" -o BatchMode=yes -o ConnectTimeout=15)

box() {
  local name=$1 ip=$2
  echo "################ $name  ($ip)"
  "${SSH[@]}" root@"$ip" '
    r=$(curl -s http://127.0.0.1:8787/api/runs 2>/dev/null | python3 -c "
import sys,json
d=json.load(sys.stdin); r=d.get(\"runs\",d) if isinstance(d,dict) else d
for x in r[-2:]: print(\"  run  \", x.get(\"id\"), \"|\", x.get(\"status\"))
" 2>/dev/null)
    echo "${r:-  run   (dashboard not answering)}"
    f=$(ls -t /home/workspace/underfit/state/runs/*.log 2>/dev/null | head -1)
    [ -n "$f" ] && echo "  step  $(tail -c 2000 "$f" | tr "\r" "\n" | grep -oE "^Step [0-9]+, Epoch [0-9]+" | tail -1)"
    [ -n "$f" ] && echo "  rate  $(tail -c 2000 "$f" | tr "\r" "\n" | grep -oE "[0-9.]+s/it" | tail -1)"
    # Match the full argv of the watcher, not the bare name: `pgrep -f queue_next.sh`
    # also matches THIS command, which contains that string -- it reported 3 watchers
    # where there was 1, and an inflated count is exactly what would hide a second
    # watcher racing the first.
    # Two mechanisms in play: box A still runs the one-shot watcher (one pending run,
    # named in QUEUED.json), box B runs the multi-run queue-runner over queue/*.json.
    # Report whichever is armed, and say plainly when NEITHER is -- an unarmed box looks
    # exactly like a healthy one from the outside until the current run ends silently.
    if [ -d /home/workspace/queue-runner.lock ]; then
      RP=$(cat /home/workspace/queue-runner.lock/pid 2>/dev/null)
      if [ -n "$RP" ] && kill -0 "$RP" 2>/dev/null; then
        echo "  queue runner pid $RP -> $(ls /home/workspace/queue/*.json 2>/dev/null | xargs -n1 basename | tr "\n" " ")"
      else
        echo "  queue !! STALE LOCK, NO RUNNER"
      fi
    elif [ -n "$(pgrep -cxf "bash /home/workspace/queue_next.sh")" ] \
         && [ "$(pgrep -cxf "bash /home/workspace/queue_next.sh")" != "0" ]; then
      echo "  queue $(python3 -c "import json;print(json.load(open(\"/home/workspace/QUEUED.json\"))[\"name\"])" 2>/dev/null || echo none) (one-shot watcher)"
    else
      echo "  queue !! NOTHING ARMED"
    fi
    echo "  gpu   $(nvidia-smi --query-gpu=utilization.gpu,memory.used --format=csv,noheader)"
    echo "  disk  $(df -h /home | tail -1 | awk "{print \$4\" free of \"\$2}")"
  '
}
box "A  ludwig -> ametsub" 217.18.55.28
box "B  cortini -> ryuichi" 217.18.55.56
