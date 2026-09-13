#!/usr/bin/env python3
"""Persistent SA3 inference worker — load the model once, generate many times.

mira (or any host) spawns this, keeps it alive, and talks JSON Lines over
stdin/stdout. The point is the 44 s DiT load measured on an M1 Pro: paying it
per generation makes a generate button feel worse than the gradio page, so it
is paid once at startup instead.

Protocol — one JSON object per line, both directions.

  → {"id": 1, "cmd": "ping"}
  ← {"id": 1, "ok": true, "ready": true, "dit": "medium", ...}

  → {"id": 2, "cmd": "generate", "prompt": "zvq, Moods: dark, epic",
     "seconds": 30, "steps": 8, "seed": 26, "cfg": 1.0,
     "lora": "/path/ckpt.safetensors", "strength": 0.7,
     "out": "/path/out.wav"}
  ← {"id": 2, "ok": true, "out": "...", "seconds_audio": 30.0,
     "timings": {...}, "wall_ms": 9800}

  → {"id": 3, "cmd": "pre_encode", "audio_dir": "/path/to/wavs",
     "output_dir": "/path/to/latents", "max_duration": 600}
  ← {"id": 3, "ok": true, "encoded": 38, "skipped": 0, "errors": 0, ...}

  → {"id": 4, "cmd": "quit"}

stdout carries ONLY protocol lines. Every log, progress line and traceback goes
to stderr, so a host can parse stdout without filtering.

Sampling itself is not reimplemented here: this delegates to sa3_gradio's
run_generation, which already handles cfg/apg, audio2audio, inpainting and the
LoRA step plan. sa3_gradio imports gradio lazily (inside build_ui), so importing
it costs nothing beyond numpy/PIL.

Run:  .venv/bin/python sa3_worker.py [--dit medium] [--decoder same-l] [--warm 30]
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
import traceback
from pathlib import Path

STUDIO = Path(__file__).resolve().parent
MLX = STUDIO / "stable-audio-3" / "optimized" / "mlx"
# run_generation and the model caches resolve their own relative paths against
# the mlx repo root, and sa3_gradio imports sibling modules by bare name.
sys.path.insert(0, str(MLX / "scripts"))
sys.path.insert(0, str(MLX))
os.chdir(MLX)


def log(msg: str) -> None:
    print(f"[worker] {msg}", file=sys.stderr, flush=True)


def emit(obj: dict) -> None:
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


class Worker:
    def __init__(self, dit: str, decoder: str):
        self.dit = dit
        self.decoder = decoder
        self.generations = 0
        self._encoder = None          # lazily loaded by pre_encode
        self._encoder_codec = None
        t0 = time.time()
        # Imported here, not at module scope, so an import failure can be
        # reported over the protocol instead of dying before the first line.
        import mlx.core as mx
        from sa3_gradio import run_generation, get_dit, get_t5, get_decoder
        from sa3_mlx import SAMPLE_RATE, SAMPLES_PER_LATENT, save_wav
        self.mx = mx
        self._run_generation = run_generation
        self._get_dit, self._get_t5, self._get_decoder = get_dit, get_t5, get_decoder
        self._save_wav = save_wav
        self.SAMPLE_RATE, self.SAMPLES_PER_LATENT = SAMPLE_RATE, SAMPLES_PER_LATENT
        mx.set_default_device(mx.gpu)
        log(f"imports {(time.time() - t0) * 1000:.0f} ms")

    def t_lat(self, seconds: float) -> int:
        import math
        return max(1, math.ceil(seconds * self.SAMPLE_RATE / self.SAMPLES_PER_LATENT))

    def warm(self, seconds: float, steps: int) -> float:
        """Pay the model load up front.

        NOTE: sa3_gradio caches the DiT under (dit_name, T_lat) and keeps only
        2. T_lat is derived from `seconds`, so generating at a *different*
        duration reloads the DiT (~44 s). Warm at the duration you intend to
        use, and keep duration fixed while A/B-ing prompts, seeds or strength.
        """
        t0 = time.time()
        self._get_t5()
        self._get_decoder(self.decoder)
        self._get_dit(self.dit, self.t_lat(seconds), self.mx.float16,
                      lora_specs=None, num_steps=steps)
        dt = time.time() - t0
        log(f"warm at {seconds}s / {steps} steps → {dt:.1f}s")
        return dt

    def lora_specs(self, req: dict):
        """Accept either one `lora`/`strength` pair or a `loras` list of specs.

        Changing strength or the step range on an already-loaded DiT is a
        ~26/80 ms in-place swap, NOT a reload (see _reconcile_lora) — so
        strength sweeps on a warm worker are effectively free.
        """
        if req.get("loras"):
            specs = req["loras"]
        elif req.get("lora"):
            specs = [{"path": req["lora"],
                      "strength": req.get("strength", 1.0),
                      "steps": req.get("lora_steps")}]
        else:
            return None
        out = []
        for s in specs:
            p = str(Path(s["path"]).expanduser())
            if not Path(p).is_file():
                raise FileNotFoundError(f"LoRA not found: {p}")
            # resolve_steps (lora_merge.py:465) unpacks `lo, hi = raw`, so steps must be
            # a 2-tuple of 1-based ints or None -- NOT a "1-8" string, which unpacks as
            # three characters. None on either side means "from the start"/"to the end",
            # matching sa3_gradio's _lora_specs_from_ui.
            raw = s.get("steps")
            steps = None
            if isinstance(raw, (list, tuple)) and len(raw) == 2:
                lo = int(raw[0]) if raw[0] else None
                hi = int(raw[1]) if raw[1] else None
                steps = None if (lo is None and hi is None) else (lo, hi)
            out.append({"path": p,
                        "strength": float(s.get("strength", 1.0)),
                        "steps": steps})
        return out

    def generate(self, req: dict) -> dict:
        out_path = req.get("out")
        if not out_path:
            raise ValueError("generate requires 'out' (destination .wav path)")
        out_path = str(Path(out_path).expanduser())
        Path(out_path).parent.mkdir(parents=True, exist_ok=True)

        seconds = float(req.get("seconds", 30.0))
        steps = int(req.get("steps", 8))
        t0 = time.time()
        audio, timings = self._run_generation(
            dit_name=req.get("dit", self.dit),
            decoder_name=req.get("decoder", self.decoder),
            prompt=req.get("prompt", ""),
            negative_prompt=req.get("negative_prompt", ""),
            seconds=seconds,
            steps=steps,
            seed=int(req.get("seed", 0)),
            cfg=float(req.get("cfg", 1.0)),
            apg=float(req.get("apg", 1.0)),
            sigma_max=float(req.get("sigma_max", 1.0)),
            a2a_audio_path=req.get("init_audio"),
            inpaint_audio_path=req.get("inpaint_audio"),
            inpaint_range_sec=req.get("inpaint_range"),
            lora_specs=self.lora_specs(req),
        )
        self._save_wav(out_path, audio, self.SAMPLE_RATE)
        self.generations += 1
        return {"out": out_path,
                "seconds_audio": round(audio.shape[-1] / self.SAMPLE_RATE, 2),
                "steps": steps,
                "timings": {k: round(float(v), 1) for k, v in (timings or {}).items()
                            if isinstance(v, (int, float))},
                "wall_ms": int((time.time() - t0) * 1000),
                "generations": self.generations}

    def pre_encode(self, req: dict) -> dict:
        """Encode a folder of audio into SAME latents for LoRA training.

        This is the step that makes embedding Python worth it: pre-encoding runs
        LOCALLY even when training runs on a remote GPU, so a C++ port of
        inference alone would not remove the Python dependency (PACKAGING.md §2).

        RAM note: this loads the *encoder* (~1.7 GB for same-l), which is a
        different model from the decoder inference uses. Running it in a worker
        that already holds the DiT pushes peak RAM up — on a 16 GB machine,
        prefer a worker dedicated to encoding, then start a fresh one to generate.
        """
        import pre_encode_mlx as pe
        audio_dir = str(Path(req["audio_dir"]).expanduser())
        output_dir = str(Path(req["output_dir"]).expanduser())
        codec = req.get("codec", self.decoder)
        if not Path(audio_dir).is_dir():
            raise NotADirectoryError(f"audio_dir not found: {audio_dir}")
        if self._encoder is None or self._encoder_codec != codec:
            t0 = time.time()
            # load_codec_encoder returns (encoder, pad_modulo); run() derives
            # pad_modulo itself from SAME_ENCODER_PAD_MODULO, so keep the model only.
            self._encoder, _pad_modulo = pe.load_codec_encoder(codec)
            self._encoder_codec = codec
            log(f"encoder {codec} loaded in {time.time() - t0:.1f}s")
        t0 = time.time()
        stats = pe.run(audio_dir, output_dir, codec, self._encoder,
                       max_duration=float(req.get("max_duration", 600.0)),
                       overwrite=bool(req.get("overwrite", False)))
        return {**stats, "output_dir": output_dir, "codec": codec,
                "wall_ms": int((time.time() - t0) * 1000)}

    def handle(self, req: dict) -> dict:
        cmd = req.get("cmd")
        if cmd == "ping":
            return {"ready": True, "dit": self.dit, "decoder": self.decoder,
                    "generations": self.generations,
                    "peak_ram_gb": round(self.mx.get_peak_memory() / 1e9, 2)}
        if cmd == "generate":
            return self.generate(req)
        if cmd == "pre_encode":
            return self.pre_encode(req)
        if cmd == "warm":
            return {"warm_s": round(self.warm(float(req.get("seconds", 30.0)),
                                              int(req.get("steps", 8))), 1)}
        raise ValueError(f"unknown cmd: {cmd!r}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dit", default="medium")
    ap.add_argument("--decoder", default="same-l",
                    help="MUST match the encoder the latents were made with")
    ap.add_argument("--warm", type=float, default=0.0, metavar="SECONDS",
                    help="Preload models for this clip length before accepting "
                         "commands (0 = load lazily on first generate)")
    ap.add_argument("--warm-steps", type=int, default=8)
    args = ap.parse_args()

    try:
        w = Worker(args.dit, args.decoder)
        if args.warm:
            w.warm(args.warm, args.warm_steps)
    except Exception as e:                       # startup failure is fatal
        traceback.print_exc(file=sys.stderr)
        emit({"id": None, "ok": False, "fatal": True, "error": str(e)})
        return 1

    emit({"id": None, "ok": True, "event": "ready",
          "dit": args.dit, "decoder": args.decoder, "warm": bool(args.warm)})

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except json.JSONDecodeError as e:
            emit({"id": None, "ok": False, "error": f"bad JSON: {e}"})
            continue
        rid = req.get("id")
        if req.get("cmd") == "quit":
            emit({"id": rid, "ok": True, "event": "bye"})
            return 0
        try:
            emit({"id": rid, "ok": True, **w.handle(req)})
        except Exception as e:
            # One bad request must never take the worker down — the whole
            # point is that the 44 s load survives a user's mistake.
            traceback.print_exc(file=sys.stderr)
            emit({"id": rid, "ok": False, "error": f"{type(e).__name__}: {e}"})
    return 0


if __name__ == "__main__":
    sys.exit(main())
