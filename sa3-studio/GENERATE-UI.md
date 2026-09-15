# The SA3 Generate window — what's missing and why

Design note, 2026-09-14. Written after the first four LoRAs (`zvq` Dune, `xyr`
Mad Max, `dkt` Dark Knight, `lrt` LOTR) were trained and auditioned, and the
window turned out to be the bottleneck — not the training.

## The finding that started this

> "same prompt, same seed gives almost the same output for all the LoRAs we built"

That should not happen. Four LoRAs trained on four different scores should pull a
fixed seed in four visibly different directions. The cause is in the window, not
the adapters.

`sa3_worker.py` accepts **`cfg`, `apg`, `negative_prompt`, `sigma_max`**.
`GenerateWindow.cpp` sends **none of them** (see the request built at
GenerateWindow.cpp:435-453). So every generation this project has ever run used:

```python
cfg  = float(req.get("cfg",  1.0))   # sa3_worker.py:161 -- guidance OFF
apg  = float(req.get("apg",  1.0))
```

`cfg = 1.0` means classifier-free guidance is neutral: the gap between "what the
prompt asks for" and "what the model would do anyway" is never amplified. Every
adapter therefore lands close to the base model's default behaviour, and the seed
— the one thing that *is* varying — dominates the result.

Ruled out as the cause: the trigger token. The user confirms they always type it.
Worth noting anyway that `triggerEditor` is **dataset-prep only**
(GenerateWindow.cpp:509, inside `chooseEncodeFolder`) and is never prepended to a
generation prompt automatically.

## What to add

### 1. cfg — the missing dial (priority)

The "how hard does it follow the prompt" control. Everything else on this list is
comfort; this one changes output.

Range 1.0-10.0, default ~4.0. At 1.0 the LoRA is a suggestion; by 4-7 it is an
instruction. Too high goes brittle and artefact-y, which is what `apg` is for.

### 2. apg — pairs with cfg

Adaptive projected guidance. It is what lets cfg go high without the harshness,
so exposing cfg without apg hands the user half a tool. Keep the two adjacent.

### 3. Negative prompt

A plain text field. Free — the backend already takes it. The obvious first use is
steering *away* from vocals on a corpus that has them (see the NIN discussion),
and away from "song structure" when what is wanted is score.

### 4. Prepend-trigger checkbox

Next to the LoRA slot picker. Selecting the `dkt` adapter should be able to put
`dkt, ` at the front of the prompt on its own. Not the cause of the bug above, but
it removes a whole class of silent mistake, and it makes the relationship between
"slot" and "token" visible to someone who did not train the thing.

### 5. Seed: lock / randomise / +1

Checkpoints get compared constantly and the comparison is only valid at a fixed
seed. Make "fixed" a visible state rather than a thing the user has to remember.

### 6. Steps default is a preview setting

`steps: 8` (sa3_worker.py:150) is a sketching value. A LoRA judged at 8 steps is
being judged through frosted glass. Raise the default and label the field so the
tradeoff reads at a glance: fast sketch vs. real render.

### 7. A/B panel

One prompt rendered across N seeds, or across N checkpoints, into a row that can
be auditioned in place. This is the feature that would actually have answered
"which of dkt's 10 checkpoints is best" in ten minutes instead of an evening.

## The bigger point: language, not jargon

The window currently presents `cfg`, `apg`, `sigma_max`, `strength`, `min/max
step` — the names the sampler uses. None of them tell a musician what will happen
to the sound. Every control should carry a plain-language line:

| Control | Not this | This |
|---|---|---|
| cfg | "Classifier-free guidance scale" | "How strictly it follows your words. Low = loose and surprising, high = literal." |
| strength | "LoRA scale 0-2" | "How much of the trained style. 0 = base model, 1 = as trained, above = exaggerated." |
| min/max step | "Step gating range" | "Early steps build the structure, late steps colour the sound. Limit the style to one or the other." |
| steps | "Sampler steps" | "8 = rough sketch. 50+ = what it really sounds like." |
| seed | "RNG seed" | "The random starting point. Same seed + same prompt = the same result every time. Lock it when comparing." |

Sliders should also *do* something legible: show the value, snap sensibly, and
sit next to their explanation rather than under an abbreviation.

## Status

Implemented 2026-09-15 (commit `fb28806`): **1 cfg, 2 apg, 3 negative prompt,
4 prepend-trigger, 5 seed randomise, 6 steps default, and the plain-language pass.**

The cfg hypothesis was confirmed by measurement before shipping, not assumed. Same
prompt, same seed 26, same LoRA (`xyr-step1000-epoch76`), same 8 steps, only cfg
differing:

```
correlation cfg1 vs cfg7   0.2354
rms                        0.1384 -> 0.3657   (2.6x fuller)
```

Bypassing the LoRA entirely at a fixed seed gives correlation 0.399 — so **cfg moved
the output more than the adapter itself did.** At cfg 1.0 the guidance term was
swamping every LoRA's contribution, which is the whole explanation for "all four
sound the same", and nothing was ever wrong with the training.

**Still open: 7, the A/B panel** — one prompt across N seeds or N checkpoints,
auditioned in place. It is the one item that needs new UI rather than a new control,
and it is the one that would have picked `dkt`'s best checkpoint in ten minutes.
