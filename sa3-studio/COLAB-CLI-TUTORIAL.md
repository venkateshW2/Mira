# Colab from the terminal — a tutorial

For someone who has never used the Colab CLI. Written 2026-09-13, from the session that
set up Dune LoRA training on an L4.

Companion docs: [COLAB-TRAINING.md](COLAB-TRAINING.md) (what we ran and why),
[RUNBOOK-dune-lora.md](RUNBOOK-dune-lora.md) (the local run).

---

## 1. What Colab actually is

Colab rents you a **computer in Google's data centre** — with a GPU. That's the whole idea.
Normally you talk to that computer through a notebook in your browser. But the computer
itself doesn't care how you talk to it.

There are **two doors into the same room**:

| Door | What it looks like |
|---|---|
| **Notebook** (browser) | Cells you click Run on. Visual, familiar. |
| **CLI** (terminal) | Commands you type. Scriptable, and an AI agent can drive it. |

Same VM, same GPU, same files. **You can use both at once.** Pick whichever suits the moment.

> **Key idea:** the CLI doesn't *replace* the notebook. It gives you another way in — and
> as you'll see in §5, it can *produce* a notebook from what you did.

---

## 2. "If there's no notebook, how do I see anything?"

This is the question everyone asks first. You are not flying blind. There are **five** ways
to see what's happening.

### (a) `colab exec` — run something, get output back
The everyday one. You pipe Python in, the output prints in your terminal.

```bash
echo 'print("hello from the GPU")' | colab exec -s sa3
```

### (b) `colab console` — watch it live ⭐
Opens a real interactive terminal **on the VM** (tmux). Training output scrolls past exactly
as it would locally. This is the "I want to watch it happen" answer.

```bash
colab console -s sa3
```

### (c) `colab ssh` — a normal SSH shell
A full shell on the VM. `ls`, `htop`, `nvidia-smi`, `tail -f` — all the usual tools.

```bash
colab ssh -s sa3
```

### (d) `colab url --open` — open it in the browser
Prints (or opens) a browser URL connected to your running session. **The CLI-created VM is
still a normal Colab session** — you can go look at it in the browser whenever you want.

```bash
colab url -s sa3 --open
```

### (e) `colab log` — export what you did as a notebook ⭐
Everything you ran through `colab exec` is recorded. Export it as a real `.ipynb`:

```bash
colab log -s sa3 -o session.ipynb     # a Jupyter notebook
colab log -s sa3 -o session.md        # or Markdown
colab log -s sa3 -n 20                # or just print the last 20 entries
```

**So "the CLI makes no notebook" isn't quite true — it makes one on request, afterwards,
from what actually ran.**

---

## 3. The commands we actually used, in order

Every command below was run for real. Copy them for the next dataset.

### Step 0 — install the CLI (once per machine)

⚠️ **Install from git, not PyPI.** The PyPI build is broken: it needs a Google *fork* of
`jupyter-kernel-client`, and a PyPI install silently pulls the wrong one, so every
`colab exec` dies with `AttributeError: module 'jupyter_kernel_client' has no attribute
'KernelClient'`.

```bash
uv tool install --force git+https://github.com/googlecolab/google-colab-cli.git
colab version        # expect 0.7.0 or newer
```

### Step 1 — log in (once)

```bash
colab sessions
```

First run prints a Google URL. Open it, approve, paste the code back **into your own
terminal**. Credentials are saved to `~/.config/colab-cli/sessions.json`.

> The Colab Pro subscription, this login, and the Drive you mount must all be the
> **same Google account**. Ours is `w2@2w12.one`.

### Step 2 — rent a GPU

```bash
colab new -s sa3 --gpu L4
```

- `-s sa3` names the session, so later commands know which VM you mean.
- `--gpu` options: `T4`, `L4`, `G4`, `A100`, `H100`. **Bigger GPU = faster = burns your
  monthly compute units faster.** L4 (23 GB VRAM) is the sweet spot for sa3-medium.
- `--high-mem` asks for more RAM (needs Pro; ignored on L4, which has one shape).

**From this moment you are spending compute units.** Check it came up:

```bash
colab status -s sa3
# [sa3] gpu-l4-... | Hardware: L4 | Shape: Standard | Variant: GPU | Status: IDLE
```

### Step 3 — get your dataset onto the VM

⚠️ **`colab upload` fails on big files.** 125 MB died instantly with an SSL error; 20 MB
works. Also **the destination folder must already exist.** So: split, upload, reassemble,
and verify.

```bash
# 1. split locally into 20 MB pieces
split -b 20m dune-ost-latents-same-l.zip chunk_

# 2. make the destination folder on the VM
echo 'import os; os.makedirs("/content/chunks", exist_ok=True)' | colab exec -s sa3

# 3. upload each piece
for f in chunk_*; do colab upload -s sa3 "$f" "/content/chunks/$(basename $f)"; done

# 4. glue them back together, CHECK THE MD5, unzip
md5 -q dune-ost-latents-same-l.zip          # local fingerprint — compare to remote
```
```python
# run via colab exec
import glob, hashlib, os, subprocess
with open('/content/dune-latents.zip','wb') as out:
    for p in sorted(glob.glob('/content/chunks/chunk_*')):
        out.write(open(p,'rb').read())
print(hashlib.md5(open('/content/dune-latents.zip','rb').read()).hexdigest())
os.makedirs('/content/latents', exist_ok=True)
subprocess.run(['unzip','-q','-o','/content/dune-latents.zip','-d','/content/latents'])
```

**Always compare the two MD5s.** A half-transferred file won't announce itself — it'll just
train badly, and you'd never know why.

### Step 4 — install the software on the VM

```python
# via colab exec
import subprocess
for repo in ["https://github.com/dada-bots/underfit",
             "https://github.com/Stability-AI/stable-audio-3"]:
    subprocess.run(["git","clone","--depth","1",repo], cwd="/content")

subprocess.run(["./install.sh","--no-setup"], cwd="/content/underfit")
```

### Step 5 — the HuggingFace token

The model weights live on HuggingFace and some are **gated**.

| Repo | Gated? | Needed for |
|---|---|---|
| `stable-audio-3-medium-base` | no | training |
| `stable-audio-3-medium` (ARC) | **yes** | demos / fast inference |

For the gated one you must click **"Agree and access repository"** at
https://huggingface.co/stabilityai/stable-audio-3-medium once, with your HF account. A
token alone is not enough — you get `GatedRepoError: 403`. One click covers all three ARC repos.

Then set the token. **Never paste a token into a chat window:**

```bash
read -rs HF_TOKEN      # paste it here; it won't echo
echo "import os; os.environ['HF_TOKEN']='$HF_TOKEN'; print('set')" | colab exec -s sa3
```

### Step 6 — download the models

⚠️ **Long jobs must run detached.** `colab exec` gives up after a few minutes with
`TimeoutError: Timeout waiting for reply` — *but the VM keeps working*. Ours "failed" and had
in fact downloaded all 14 GB. So launch in the background and poll:

```python
import subprocess, os
subprocess.Popen(
    "cd /content/underfit && nohup uv run python -m underfit.cli.setup "
    "--backend sa3 --backend-path /content/stable-audio-3 --models sa3-medium "
    "> /content/setup.log 2>&1 &",
    shell=True, env=dict(os.environ))
```

Check on it whenever you like:

```python
import subprocess
print(subprocess.run("tail -5 /content/setup.log; pgrep -fc underfit.cli.setup || echo DONE",
                     shell=True, capture_output=True, text=True).stdout)
```

### Step 7 — train, and watch

```bash
colab console -s sa3        # live view — training scrolls past
```

### Step 8 — bring the LoRA home

```bash
colab download -s sa3 /content/underfit/state/runs/<run>/checkpoints/<file>.safetensors ./dune-zvq.safetensors
```

A LoRA is small (~36 MB), so this is quick.

### Step 9 — ⚠️ STOP THE VM

```bash
colab stop -s sa3
```

**This is the one that stops the spending.** The CLI runs a keep-alive daemon so the VM
will *not* time out on its own — that's great for long training, and expensive if you
forget. Make it a habit.

```bash
colab sessions     # confirm nothing is running
```

---

## 4. Where Google Drive fits

Short answer: **for our workflow, you don't strictly need it.**

The VM has two kinds of storage:

| Storage | Speed | Survives session end? |
|---|---|---|
| VM local disk (`/content`) | fast (~500 MB/s) | ❌ **wiped** |
| Google Drive (mounted) | slow (~30 MB/s) | ✅ **kept** |

Because `colab upload` puts files straight onto the VM, Drive is optional. Its real use is
**persistence** — anything you want to survive the VM being destroyed.

```bash
colab drivemount -s sa3           # appears at /content/drive
```

A sensible split (this is what underfit's own notebook does):

- **Drive:** trained LoRAs, run history, anything you'd cry about losing.
- **VM disk:** model weights (14 GB — re-downloads in minutes, not worth Drive space),
  latents, scratch files.

Our Drive account: `w2@2w12.one`, folder `Colab Notebooks`
(`1dl3xCSXfz4q8MLoVYx7AmgFW2ecD6WHt`).

> **Don't put the model weights on Drive.** Reading 14 GB at 30 MB/s is far slower than
> re-downloading from HuggingFace.

---

## 5. "What if I want a proper notebook?"

Three options, easiest first.

### Option A — export what you already did
```bash
colab log -s sa3 -o my-session.ipynb
```
Turns your CLI history into a real notebook. Good for "make me a repeatable version of
what we just figured out."

### Option B — use underfit's ready-made notebook
`underfit/underfit-colab.ipynb` is written and maintained by Dadabots for exactly this.
Upload it to Drive, open in Colab, set **Runtime → Change runtime type → GPU**, and work
through Steps 1–8. It does everything §3 above does, with buttons.

This is the better choice if you want the **web dashboard** with loss curves and demo
playback.

### Option C — run a local notebook file on the VM
```bash
colab exec -s sa3 -f my-notebook.ipynb
```
Executes a notebook from your laptop on the remote GPU, writing outputs back.

### Which should you use?

| You want | Use |
|---|---|
| Watch loss curves, click through demos | **Notebook** (Option B) |
| Repeatable, scriptable, agent-drivable | **CLI** |
| Both | Run the CLI, and `colab url --open` to view the same session in a browser |

They are not exclusive. The VM the CLI created is a normal Colab session.

---

## 6. Command cheat-sheet

```bash
# session
colab new -s NAME --gpu L4       # rent a GPU  (STARTS SPENDING)
colab sessions                   # what's running
colab status -s NAME             # hardware + state
colab stop -s NAME               # STOP SPENDING
colab url -s NAME --open         # open in browser

# running things
echo 'print(1)' | colab exec -s NAME     # run python from stdin
colab exec -s NAME -f script.py          # run a local file
colab console -s NAME                    # live TTY (watch training)
colab ssh -s NAME                        # full shell

# files
colab ls -s NAME /content
colab upload -s NAME local.txt /content/remote.txt     # <=20 MB, dir must exist
colab download -s NAME /content/out.bin ./out.bin

# other
colab install -s NAME numpy pandas
colab drivemount -s NAME
colab log -s NAME -o session.ipynb       # export history as a notebook
colab pay                                # manage compute units
```

---

## 7. Things that will bite you

1. **Forgetting `colab stop`.** The keep-alive daemon means the VM never times out. Check
   `colab sessions` before bed.
2. **Installing the CLI from PyPI.** Broken — use the git install (§3 Step 0).
3. **Uploading a file over ~20 MB.** Fails with an unhelpful SSL error. Chunk it.
4. **Uploading into a folder that doesn't exist.** Fails with no useful message.
5. **Assuming a long job died** because `colab exec` timed out. It usually hasn't — check
   the VM before re-running, or you'll download 14 GB twice.
6. **A gated HF repo.** A valid token is not access. Click "Agree" in a browser, once.
7. **Reaching for A100 by default.** Units drain fastest there. Start on L4.
8. **The CLI can lose your session handle — and then you cannot stop the VM.**
   `~/.config/colab-cli/sessions.json` maps your nickname (`sa3`) to the real VM. We saw it
   silently reset to `{}` mid-session — after which `colab sessions` shows the VM as `[?]`
   and **every** `-s` command fails with *Session not found*, including `colab stop`. You
   cannot re-attach by the backend id, and `colab new -s sa3` does **not** reclaim it — it
   quietly creates a *second* session (ours came up as CPU because we omitted `--gpu`), so
   you end up paying for two.

   **Recovery:** kill it from the browser — https://colab.research.google.com/ →
   **Runtime → Manage sessions → Terminate**. That UI is the source of truth; the CLI list
   can be stale in both directions (ours kept listing an L4 that Colab had already
   reclaimed).

   **The Resources panel in Colab shows your live burn rate** ("approximately N compute
   units per hour"). When in doubt about whether you're being charged, trust that, not the
   CLI.

9. **`/content` is temporary.** Anything you want to keep goes to Drive or gets downloaded
   before you stop the VM.
