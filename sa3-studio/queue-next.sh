#!/bin/bash
# queue-next.sh — launch the queued run when the current one finishes.
#
# Completion-triggered, never a timer: "hold buffer for the queues, only after one
# finishes the other starts". A timer would either start early onto a busy GPU or waste
# an hour of a rented box waiting for a run that had already ended.
#
# Detached (setsid, </dev/null) so it survives the SSH session that armed it. Arming it
# with a plain `nohup ... &` inside an ssh command does NOT detach: the channel stays
# open, the call hangs, and the caller cannot tell whether the copy of QUEUED.json even
# ran. That is how one box ended up armed with the previous session's run.
LOG=/home/workspace/queue.log

# Refuse to be the second watcher. Two of these racing both POST when training exits,
# which starts the same run twice on one GPU. It has happened: a hung arming call landed
# minutes late and left two armed at once, and the only reason it was caught is that the
# status script counts them.
ME=$$
OTHERS=$(pgrep -xf "bash /home/workspace/queue_next.sh" 2>/dev/null | grep -v "^${ME}$")
if [ -n "$OTHERS" ]; then
  echo "[$(date -u)] another watcher already armed (pids: $(echo $OTHERS | tr '\n' ' ')); exiting" >> $LOG
  exit 0
fi

echo "[$(date -u)] armed, waiting for lora_train.py to exit" >> $LOG
while pgrep -f lora_train.py >/dev/null; do sleep 60; done
echo "[$(date -u)] training exited; settling 180s before launch" >> $LOG
sleep 180
if pgrep -f lora_train.py >/dev/null; then
  echo "[$(date -u)] ABORT: something restarted training, not launching" >> $LOG; exit 0
fi
R=$(curl -s -X POST http://127.0.0.1:8787/api/runs/new -H 'Content-Type: application/json' -d @/home/workspace/QUEUED.json)
echo "[$(date -u)] launch response: $R" >> $LOG
