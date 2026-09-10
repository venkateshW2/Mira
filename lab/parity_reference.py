#!/usr/bin/env python3
"""Phase 0 day 3 spike (PRD §9): numerical-parity reference.

Reproduces the mel-frontend + patching stage of Essentia's
TensorflowPredictEffnetDiscogs by hand (frameSize=512, hopSize=256, sampleRate=16000,
numberBands=96, patchSize=128, patchHopSize=62 — read directly from
vendor/essentia/src/algorithms/machinelearning/tensorflowpredicteffnetdiscogs.h,
not guessed), since that composite algorithm itself needs TensorflowPredict, which needs
libtensorflow, which mira's no-TF build does not have. We reimplement the pre/post steps
around ONNX Runtime instead — this script is the Python side of that reimplementation;
spike/02_onnx_parity is the C++ side. They must agree to ~1e-4 (PRD §9 day 3).

Usage:
    uv run python3 parity_reference.py <audio-file> <out-prefix>

Writes <out-prefix>_patches.bin (n*128*96 float32) and <out-prefix>_embeddings.bin
(n*1280 float32), both raw row-major float32, plus a .shape.txt with the value of n.
"""
import sys
import numpy as np
import essentia.standard as es
import onnxruntime as ort

FRAME_SIZE = 512
HOP_SIZE = 256
SAMPLE_RATE = 16000
NUMBER_BANDS = 96
PATCH_SIZE = 128
PATCH_HOP_SIZE = 62

MODEL_PATH = "../models/feature-extractors/discogs-effnet/discogs-effnet-bsdynamic-1.onnx"


def compute_patches(audio_path: str) -> np.ndarray:
    loader = es.MonoLoader(filename=audio_path, sampleRate=SAMPLE_RATE)
    audio = loader()

    frame_cutter = es.FrameCutter(frameSize=FRAME_SIZE, hopSize=HOP_SIZE)
    tf_input = es.TensorflowInputMusiCNN()

    bands = []
    while True:
        frame = frame_cutter(audio)
        if not len(frame):
            break
        bands.append(tf_input(frame))
    bands = np.array(bands, dtype=np.float32)  # [n_frames, 96]

    n_frames = bands.shape[0]
    n_patches = 1 + (n_frames - PATCH_SIZE) // PATCH_HOP_SIZE
    if n_patches < 1:
        raise RuntimeError(f"file too short: only {n_frames} mel frames, need >= {PATCH_SIZE}")

    patches = np.stack([
        bands[i * PATCH_HOP_SIZE : i * PATCH_HOP_SIZE + PATCH_SIZE]
        for i in range(n_patches)
    ]).astype(np.float32)  # [n_patches, 128, 96]

    return patches


def compute_embeddings(patches: np.ndarray) -> np.ndarray:
    sess = ort.InferenceSession(MODEL_PATH, providers=["CPUExecutionProvider"])
    input_name = sess.get_inputs()[0].name
    outputs = sess.run(None, {input_name: patches})
    # PartitionedCall:0 = predictions [n,400], PartitionedCall:1 = embeddings [n,1280]
    for name, out in zip([o.name for o in sess.get_outputs()], outputs):
        if out.shape[-1] == 1280:
            return out
    raise RuntimeError("could not find the 1280-dim embedding output")


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <audio-file> <out-prefix>", file=sys.stderr)
        sys.exit(1)

    audio_path, out_prefix = sys.argv[1], sys.argv[2]

    patches = compute_patches(audio_path)
    print(f"patches: {patches.shape}")
    embeddings = compute_embeddings(patches)
    print(f"embeddings: {embeddings.shape}")

    patches.tofile(f"{out_prefix}_patches.bin")
    embeddings.astype(np.float32).tofile(f"{out_prefix}_embeddings.bin")
    with open(f"{out_prefix}.shape.txt", "w") as f:
        f.write(f"n_patches={patches.shape[0]}\n")

    print(f"wrote {out_prefix}_patches.bin, {out_prefix}_embeddings.bin")


if __name__ == "__main__":
    main()
