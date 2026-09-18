#!/bin/bash
# queue-runner.sh — run every queued LoRA, one at a time, in filename order.
#
# Supersedes queue-next.sh, which held exactly ONE pending run. Box B has three in a
# row (cortini -> ryuichi -> dune) and a one-shot watcher cannot express that: arming
# the third would mean being awake when the second finished, which is the thing the
# queue exists to avoid.
#
# Completion-triggered throughout: "hold buffer for the queues, only after one finishes
# the other starts". Never a timer.
#
# Queue files live in /home/workspace/queue/*.json and are launched in `sort` order, so
# name them 10-foo.json, 20-bar.json. A launched file is renamed .launched rather than
# deleted, so the queue is its own record of what ran and in what order.
LOG=/home/workspace/queue.log
Q=/home/workspace/queue
SETTLE=180          # let the GPU actually free before the next run allocates
APPEAR=150          # ...and let a just-launched run become a visible process

# Single instance, via an ATOMIC lock rather than a process scan. pgrep cannot be used
# here: `pgrep -f queue-runner.sh` matches the shell that is LAUNCHING queue-runner.sh,
# because that shell's command line contains the string it is searching for. That is not
# hypothetical -- it fired on the first launch and the runner politely exited, leaving
# the box with nothing armed at all, which is far worse than the double-launch the guard
# exists to prevent. mkdir either creates the directory or fails; there is no window.
LOCK=/home/workspace/queue-runner.lock
if ! mkdir "$LOCK" 2>/dev/null; then
  OLD=$(cat "$LOCK/pid" 2>/dev/null)
  if [ -n "$OLD" ] && kill -0 "$OLD" 2>/dev/null; then
    echo "[$(date -u)] runner already active (pid $OLD); exiting" >> $LOG
    exit 0
  fi
  # A lock with no live owner is a crash or a reboot, not a running runner. Taking it
  # over is right: the alternative is a queue that never starts and says nothing.
  echo "[$(date -u)] stale lock (pid '${OLD:-none}'); taking it over" >> $LOG
fi
echo $$ > "$LOCK/pid"
trap 'rm -rf "$LOCK"' EXIT

# Is anything actually on the GPU? A loading or training run holds gigabytes; an idle
# A30 sits near zero. 500 MiB is well clear of both.
gpu_busy() {
  local used
  used=$(nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits 2>/dev/null | head -1)
  [ -n "$used" ] && [ "$used" -gt 500 ] 2>/dev/null
}

echo "[$(date -u)] runner up; queued: $(ls $Q/*.json 2>/dev/null | wc -l)" >> $LOG

while true; do
  NEXT=$(ls $Q/*.json 2>/dev/null | sort | head -1)
  if [ -z "$NEXT" ]; then
    echo "[$(date -u)] queue empty; runner exiting" >> $LOG
    break
  fi

  # Readiness is asked of the GPU, not the process table. pgrep kept matching for 27
  # minutes after a trainer had exited and the GPU had dropped to 0% -- a finished
  # process still answers pgrep until it is reaped, and the runner sat waiting on a run
  # that was already over while the box billed for an idle A30. The GPU cannot lie about
  # whether work is on it.
  while gpu_busy; do sleep 60; done
  echo "[$(date -u)] GPU free; settling ${SETTLE}s before $(basename $NEXT)" >> $LOG
  sleep $SETTLE

  # Someone may have started a run by hand while we settled. Theirs wins; we wait again
  # rather than stacking a second one on top of it.
  if gpu_busy; then
    echo "[$(date -u)] training restarted during settle; standing down and re-waiting" >> $LOG
    continue
  fi

  R=$(curl -s -X POST http://127.0.0.1:8787/api/runs/new \
        -H 'Content-Type: application/json' -d @"$NEXT")
  echo "[$(date -u)] launched $(basename $NEXT): $R" >> $LOG
  mv "$NEXT" "$NEXT.launched"

  # Without this the loop would see an empty process table before the new run has
  # spawned lora_train.py and immediately launch the one after it.
  sleep $APPEAR
done
