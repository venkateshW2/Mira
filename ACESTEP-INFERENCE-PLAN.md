# ACE-Step inference plan

**Research + integration plan, 2026-09-16.** Status: **proposal, nothing built.**

The question this answers: *can mira generate audio without a Python sidecar?* For SA3
the answer was no and always will be — `sa3_worker.py` is the seam, and
[sa3-studio/PACKAGING.md](sa3-studio/PACKAGING.md) measured that on 2026-09-13. ACE-Step
1.5 has a C++/GGML implementation with a Metal backend, so for ACE-Step the answer is
**yes**, and somebody has already done the port.

Everything below was read from source (both repos cloned and inspected) or from named
public sources. Where a claim is somebody's marketing and has not been independently
verified, it says so.

---

## 1. What ACE-Step 1.5 is

An open-source music **foundation** model from ACE Studio and StepFun, MIT licensed.
Released late January 2026; the 4B "XL" DiT variants landed 2026-04-02.

The architecture is a hybrid the others do not have: a **Qwen3-derived language model
acts as a planner**, turning a short user query into a full song blueprint — metadata,
lyrics and captions via chain-of-thought — which then conditions a **Diffusion
Transformer**. The LM is a separate downloadable model (0.6B / 1.7B / 4B) and can be
switched off entirely, leaving the DiT to run on an explicit prompt.

| | |
|---|---|
| Output | stereo **48 kHz**, 10 s to 10 minutes |
| Speed | <2 s per full song on A100, <10 s on a 3090 |
| Footprint | **<4 GB** VRAM DiT-only; 6 GB with the LM |
| Licence | MIT |
| Modes | text2music, cover, repaint, lego (add layers), extract (stems), complete (accompany vocals) |

Beyond generation it also does **audio understanding** — extracting BPM, key/scale, time
signature and a caption from audio. That overlaps mira. See §8.

### It is a SONG model

This is the single most important framing and it is easy to miss under the benchmark
numbers. ACE-Step is built around **lyrics, vocals and verse/chorus structure**, across
50+ languages. Its own LoRA tutorial requires a `.lyrics.txt` beside every training file.
The DAW plugin's headline controls are *Creativity* and *Strictly follow lyrics*.

mira's corpus is instrumental: film score, sound design, texture, arc. That is the half
of the space SA3 was built for and ACE-Step points away from. This does not make it
useless here — it makes it a **second instrument**, not a replacement.

---

## 2. Quality, and the sentiment honestly reported

**The published numbers are good.** AudioBox CU 8.09, Production Quality 8.35, and it
tops Suno v5 on the SongEval overall metric.

**Two caveats that belong beside those numbers:**

- They originate in ACE-Step's own paper and reach the blogs through aggregation. The
  repo contains **no comparison against Stable Audio at all** — `docs/en/BENCHMARK.md` is
  purely their own speed profiler. No head-to-head could be verified.
- Suno v5 still leads **style alignment** (46.8 vs 39.1) and **lyric alignment**
  (34.2 vs 26.3).

**The criticism is consistent across every independent source**, which is what makes it
credible:

- **Vocal artefacts** — metallic timbre, sibilance distortion, pitch glitches on
  sustained notes; worst in high registers and falsetto.
- *"Limitations in vocal realism and fine-grained musical control."*
- An open issue, [#1114 "[Important] Technical Feedback on ACE-Step 1.5 XL (Music Quality
  & Expressiveness)"](https://github.com/ace-step/ACE-Step-1.5/issues/1114), means even
  the newest XL models are taking musicianly pushback.
- [Jordi Pons](https://artintech.substack.com/p/ace-step-15-explained) — a serious MIR
  researcher, not a content farm — wrote the explainer. That the field's actual
  practitioners engage with it is a better signal than any score.

**What that means for mira specifically:** nearly every published criticism is about
**vocals**, which this project does not use. That whole weakness misses us. The one that
lands is *"fine-grained musical control"* — which is precisely what the prompt builder
and the planned generation sweep exist to provide.

---

## 3. The C++ path — why this is different from SA3

Two repos, both official or officially endorsed:

| repo | what it is |
|---|---|
| [ServeurpersoCom/acestep.cpp](https://github.com/ServeurpersoCom/acestep.cpp) | **Portable C++17 + GGML implementation.** CPU / CUDA / ROCm / **Metal** / Vulkan |
| [ace-step/acestep.vst3](https://github.com/ace-step/acestep.vst3) | **JUCE 8 VST3 instrument** built on acestep.cpp. 35 source files |

`acestep.cpp` builds **`acestep-core` as a STATIC library** (`CMakeLists.txt:113`). That
is the whole story. mira is C++20/JUCE and already links Essentia and ONNX Runtime as
static/vendored dependencies; `acestep-core` is the same kind of dependency.

**On macOS the build auto-enables Metal and Accelerate BLAS** with no extra flags.

### The port is unusually well evidenced

Most GGUF conversions ship with a vibe. This one ships with numbers:

- Test logs comparing GGML C++ output against the Python reference by **cosine similarity
  per intermediate tensor**.
- **Q8 quantization error sits 39 dB below the VAE's own reconstruction error**; Q4 sits
  16 dB below and is described as inaudible on most material. The quantization is
  quieter than the decoder's noise floor — which is the right way to state a quantization
  claim, and almost nobody does.

### The one real risk

It uses a **patched GGML fork** — "two new ops, a Metal im2col optimization, and a CUDA
bugfix for the Oobleck VAE decoder." That is the same shape as the `beat_this_cpp`
problem hit on 2026-09-17: a vendored dependency carrying local patches, where a
re-fetch silently breaks the build. It is manageable — `scripts/fetch-vendor.sh` already
has the convention — but it is a maintenance cost, not a free lunch.

### And the VST3 is a reference implementation

`acestep.vst3` is a working example of *"a JUCE application drives acestep.cpp"*. 35
source files, already solving audio-thread handoff, DAW state persistence, file I/O and
an audio preview engine. The integration below is not being designed from nothing.

---

## 4. Model files and disk

`acestep.cpp` needs **four GGUFs**, from
[Serveurperso/ACE-Step-1.5-GGUF](https://huggingface.co/Serveurperso/ACE-Step-1.5-GGUF):

| type | recommended pick | size |
|---|---|---|
| LM | `acestep-5Hz-lm-4B-Q8_0.gguf` | 4.2 GB |
| Text encoder | `Qwen3-Embedding-0.6B-Q8_0.gguf` | 748 MB |
| DiT | `acestep-v15-turbo-Q8_0.gguf` | 2.4 GB |
| VAE | `vae-BF16.gguf` | 322 MB |
| | **total** | **~7.7 GB** |

Smaller LMs exist (0.6B Q8_0 = 710 MB, 1.7B Q8_0 = 1.98 GB) and the LM can be skipped
entirely for DiT-only operation, which is the likely mode here — mira writes its own
captions and does not need an LM to invent one.

DiT variants: `turbo` (8 steps), `sft` (50 steps, higher quality), `base`, `shift1`,
`shift3`, `continuous`. XL (4B) DiT at Q4_K_M is 2.99 GB.

**Do not clone the whole GGUF repo — it is 215 GB across every quantization.** Pull the
four files.

Official (non-GGUF) checkpoints are ~10 GB, per `docs/en/INSTALL.md:33`.

---

## 5. The API mira would call

Read from `src/pipeline-synth.h` and `src/request.h`. It is a clean C-style API.

```c
struct AceSynthParams {
    const char * text_encoder_path;  // Qwen3 text encoder GGUF (required)
    const char * dit_path;           // DiT GGUF (required)
    const char * vae_path;           // VAE GGUF (required)
    const char * adapter_path;       // adapter safetensors or directory (NULL to disable)
    float        adapter_scale;      // user scale multiplier
    bool         use_fa;             // flash attention
    bool         use_batch_cfg;      // batch cond+uncond in one DiT forward
    int          vae_chunk, vae_overlap;
    ...
};

AceSynth *    ace_synth_load(ModelStore *, const AceSynthParams *);
AceSynthJob * ace_synth_job_run_dit(AceSynth *, const AceRequest * reqs, ...,
                                    int batch_n,              // 1..9
                                    bool (*cancel)(void *),   // polled between DiT steps
                                    void * cancel_data);

struct AceAudio { float * samples; int n_samples; int sample_rate; };  // planar stereo, 48000
```

**Four things in that signature matter more than they look:**

1. **`adapter_path` + `adapter_scale`.** `src/adapter-merge.h` implements **LoRA, LoKr and
   DoRA** merge at load. So mira can load ACE-Step adapters directly, with a strength
   dial, exactly as `GenerateWindow` already does for SA3.
2. **`batch_n` is 1..9.** *"synth_batch_size: number of DiT variations per request."*
   The "give me two or three tracks per prompt instead of one" feature is **native**, not
   something to build on top.
3. **The `cancel` callback is polled between DiT steps.** `GenerateWindow`'s Stop button
   already exists and currently has to kill a subprocess. This is a clean cooperative
   cancel.
4. **`ModelStore` holds no GPU modules.** Each op acquires and releases its own module
   with RAII; STRICT policy keeps at most one resident. That is what makes <4 GB work,
   and it means mira does not have to manage residency itself.

### The request maps onto mira's captions almost 1:1

```c
struct AceRequest {
    std::string caption, lyrics;
    int         bpm;              // 0 = unset
    float       duration;
    std::string keyscale, timesignature, vocal_language;
    int         lm_batch_size, synth_batch_size;
    int64_t     seed;             // -1 = random
    int         inference_steps;  // 0 = auto (turbo 8, base/sft 50)
    float       guidance_scale, shift;
    ...
};
```

`caption`, `bpm`, `keyscale`, `timesignature` are **already produced by mira**, and as of
2026-09-17 the BPM is verified 6/6 against ground truth. ACE-Step's own LoRA tutorial
tells users to fetch BPM and key from a **website** (vocalremover.org) because
*"generating them directly with LM will produce hallucinations."* mira does it locally
and correctly. This is the single strongest argument that the two fit together.

---

## 6. The plan

Phased, each phase ending in something checkable. Nothing after Phase 1 should start
until Phase 1's answer is known.

### Phase 0 — Listen before building *(half a day)*

Install the Python app on the Mac and generate. **Nothing is decided until it has been
heard on this project's material.**

```bash
git clone https://github.com/ace-step/ACE-Step-1.5.git && cd ACE-Step-1.5
uv sync
./start_gradio_ui_macos.sh          # MPS for the DiT, MLX for the LM
```

Test on instrumental prompts taken straight from mira's own caption vocabulary —
a `tar`-style Reznor/Ross prompt, a `trn` TRON prompt, something Amon-shaped.

**The decision gate:** does an instrumental prompt with no lyrics produce something
worth having? Every published criticism is about vocals; nobody has reported on what this
does with a pure score prompt. If the answer is no, **stop here** — the rest of this plan
is wasted effort, and half a day is the whole cost.

### Phase 1 — `spike/07_acestep_link` *(one to two days)*

Follow the project's own Phase 0 convention ([spike/README.md](spike/README.md)): prove
the risky assumption in a standalone CMake project before touching mira.

`spike/06_embedded_python` asked *"can we embed Python for SA3?"*. This one asks
**"can we skip Python entirely?"** — the same question, six spikes later, with a
different answer available.

What it must prove, in order:

1. `acestep.cpp` builds on Apple Silicon with Metal (`./buildcpu.sh` auto-enables Metal
   and Accelerate; needs `--recurse-submodules` for the GGML fork).
2. `acestep-core` links into a bare CMake target alongside JUCE without symbol or
   C++-standard conflicts (acestep.cpp is C++17, mira is C++20).
3. One `ace_synth_job_run_dit` call from C++ produces audio identical to the `ace-synth`
   CLI on the same seed.
4. **Measure the numbers that decide everything else:** seconds per generation at 8
   steps and at 50, peak RSS, and whether the `cancel` callback actually interrupts
   mid-generation.

Record the measurements in the spike's README, as spikes 01–06 did.

### Phase 2 — `AceStepEngine` in `mira_core`? No — in `mira_ui` *(three to five days)*

A deliberate architectural note. `mira_core` is defined in
[CLAUDE.md](CLAUDE.md) as *"pure arithmetic over stored JSON: no audio, no models."* A
generation engine is emphatically not that, and putting it there would break the one
invariant that keeps the CLI and UI from disagreeing.

It belongs beside `GenerateWindow`, as a sibling to the existing worker transport:

```
src/mira_ui/Source/
  GenerateWindow.{h,cpp}     unchanged UI
  Sa3Worker.{h,cpp}          existing: JSON over stdin to Python      [SA3]
  AceStepEngine.{h,cpp}      new: direct acestep-core calls           [ACE-Step]
```

`GenerateWindow` gains a **backend selector** and routes to one or the other. Both
present the same shape — prompt, seed, steps, guidance, LoRA path, LoRA strength — because
both models genuinely take those. `cfg`/`apg` map onto `guidance_scale`/`shift`.

The generation call runs on its own thread with the `cancel` callback wired to the
existing Stop button.

### Phase 3 — the things that are free once Phase 2 lands *(two to three days)*

These are not new features. They are `AceRequest` fields already in the struct:

- **`synth_batch_size` 1..9** — the "two or three tracks per prompt" request, native.
- **`seed`** — per-request, so a seed sweep is one call with a batch, not N calls.
- **`adapter_path` / `adapter_scale`** — the LoRA slot, with strength.
- **`inference_steps` / `guidance_scale` / `shift`** — already sliders in the window.

### Phase 4 — decide, and write it down

Audition SA3 and ACE-Step on the same prompts and record the verdict the way
[ANALYSIS.md](ANALYSIS.md) records measurements. Two possible outcomes and **both are
acceptable**:

- ACE-Step becomes a second engine beside SA3, chosen per job.
- It is dropped, and the spike plus this document are the record of why.

What must **not** happen is drifting into maintaining two generation stacks without ever
deciding.

---

## 7. Risks, stated plainly

| risk | severity | note |
|---|---|---|
| **Instrumental output is weak** | **high** | The whole architecture assumes lyrics. Phase 0 exists solely to find this out cheaply |
| Patched GGML fork | medium | Same failure mode as `beat_this_cpp`; use the `scripts/vendor-patches/` convention from day one |
| C++17 vs C++20 ABI/flag mismatch | medium | Phase 1 item 2 |
| Build time and binary size | medium | GGML with Metal is a heavier dependency than ONNX Runtime |
| No SA3 head-to-head exists | medium | Nobody has published one. Phase 4 has to be our own ears |
| Two generation stacks to maintain | medium | Phase 4's forcing function |
| 7.7 GB of models | low | Comparable to what `models/` already holds |

---

## 8. What this does NOT replace

**mira's analysis stack.** ACE-Step ships `ace-understand` / `pipeline-understand.h`,
which extracts BPM, key, time signature and a caption from audio. On paper that overlaps
mira entirely. It does not in practice:

- It is **LM-based**, and their own tutorial warns that LM-generated BPM and key
  hallucinate — which is why it tells users to go to a website instead.
- mira's is **measurement-based**: Beat This + a madmom-compatible DBN + `gridStability`,
  verified 6/6 against tempos the user knows.
- mira omits rather than guesses (CLAUDE.md convention 1). An LM does the opposite by
  construction.

It is worth running `ace-understand` on the six ground-truth tracks purely as a third
opinion. It is not worth adopting.

**SA3 and the LoRAs.** Seven trained adapters, a working training rig, and a corpus
prepared for it. None of that transfers — different model, different latent space,
different adapter format. ACE-Step is an addition or it is nothing.

---

## 9. Sources

Read directly (both repos cloned 2026-09-16):

- [ace-step/ACE-Step-1.5](https://github.com/ace-step/ACE-Step-1.5) — 8,256 lines of
  English docs, incl. `LoRA_Training_Tutorial.md`, `INSTALL.md`, `GPU_COMPATIBILITY.md`
  and the bundled Side-Step guides
- [ServeurpersoCom/acestep.cpp](https://github.com/ServeurpersoCom/acestep.cpp) —
  `CMakeLists.txt`, `src/pipeline-synth.h`, `src/request.h`, `src/adapter-merge.h`

Public sources:

- [acestep.vst3](https://github.com/ace-step/acestep.vst3) · [VST3 request #890](https://github.com/ace-step/ACE-Step-1.5/issues/890) · [XL quality feedback #1114](https://github.com/ace-step/ACE-Step-1.5/issues/1114)
- [ACE-Step-1.5-GGUF](https://huggingface.co/Serveurperso/ACE-Step-1.5-GGUF)
- [Jordi Pons — ACE-Step 1.5 Explained](https://artintech.substack.com/p/ace-step-15-explained) · [Musician's Guide #235](https://github.com/ace-step/ACE-Step-1.5/discussions/235)
- [Best Open-Source AI Music Models 2026 — Boppy](https://boppy.me/blog/best-open-source-ai-music-models) · [IT-JIM](https://www.it-jim.com/blog/best-open-source-ai-music-generator/) · [DEV Community guide](https://dev.to/czmilo/ace-step-15-the-complete-2026-guide-to-open-source-ai-music-generation-522e)
- [awesome-ace-step](https://github.com/ace-step/awesome-ace-step) · [Technical report, arXiv:2602.00744](https://arxiv.org/abs/2602.00744)

---

## Appendix — training, for the record

Not part of this plan (this document is about **inference**), but found during the
research and worth keeping:

- **Official:** one-click LoRA in the Gradio UI. *"8 songs, 1 hour on a 3090."* 16 GB
  VRAM minimum, 20 GB recommended.
- **[Side-Step](https://github.com/koda-dernet/Side-Step)** by dernet is the underfit
  analogue — a standalone CLI training toolkit with a wizard and presets. It exists
  because the official trainer has **two real bugs** for non-turbo models: a discrete
  8-step timestep schedule hardcoded for turbo instead of logit-normal continuous
  sampling, and **no CFG dropout** (*"without this, inference quality suffers"*). That is
  the same shape of fault as mira's `cfg = 1.0` bug — a guidance default silently
  flattening every adapter.
- **LoKr** trains ~10x faster than LoRA: *"what used to take an hour now takes 5 minutes."*
- **Gradient estimation** ranks attention modules by how strongly they respond to *your*
  dataset, so a rank-32 adapter on 16 chosen modules can beat rank-64 across all 80+.
  This is the principled version of the "layer filter" question, and its dataset-
  comparison mode — *"run estimation on two datasets and compare"* — is a direct way to
  ask why `amt` artefacts where `nin` did not.
- **Training is CUDA-only.** `train.py` touches only `torch.cuda`; Side-Step's own docs
  say *"CPU/MPS are experimental."* Inference comes to the Mac; training stays on rented
  GPUs.
