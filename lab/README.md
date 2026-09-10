# lab/ — offline Python, never shipped (PRD §7)

A pinned `uv` environment for model conversion, numerical-parity checks against the C++
build, and quick embedding experiments. Never invoked by the mira app at runtime.

```bash
cd lab
uv sync              # creates .venv, installs pinned deps (uv manages its own Python 3.12)
uv run python3 ...
```

## What's pinned and why

- **`essentia-tensorflow==2.1b6.dev1389`** — the version-critical pin from PRD §3: the
  last release with `cp39–cp313` arm64 wheels before `2.1b6.dev1438` dropped to cp314-only.
  Re-verified 2026-09-10 on this machine — all algorithms load (see below).
- **`tf2onnx`** — converts the two MTG models not published as ONNX (PRD §16.3).
- **`onnx` / `onnxruntime`** — for the numerical-parity harness, diffing C++ ONNX Runtime
  output against the Python reference path.
- **`numpy<2` (pinned via the resolved lock, currently 1.26.4)** — matches what the
  `essentia-tensorflow` wheel was built against.

## Verified 2026-09-10

```
essentia version: 2.1-beta6-dev
RhythmExtractor2013 / KeyExtractor / TuningFrequency / PredominantPitchMelodia /
LoudnessEBUR128 / Danceability / BeatsLoudness / HPCP /
TensorflowPredictEffnetDiscogs / TensorflowPredictMusiCNN /
TensorflowPredictVGGish / TensorflowPredict2D  — all present
```

Cross-checked against the C++ spike (`spike/01_essentia_link`) on the same fixture
(`vendor/essentia/src/examples/python/musicbricks-tutorials/flamenco.wav`):

| | Python wheel | C++ static lib |
|---|---|---|
| BPM | 117.82563781738281 | 117.826 |
| confidence | 1.223110556602478 | 1.22311 |

Exact match to displayed precision — first parity signal between the two runtimes, before
any neural/mel-frontend work has happened.

## Not yet built

- The numerical-parity harness proper (mel bands + embeddings, diffed to ~1e-4 per PRD
  §9 day 3) — needs the ONNX embedding spike first.
- `tf2onnx` conversion scripts for the two non-ONNX models (§16.3).
