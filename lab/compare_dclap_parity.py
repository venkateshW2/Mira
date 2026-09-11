#!/usr/bin/env python3
"""Phase 4 spike (spike/05_dclap_parity): diff Python vs C++ DCLAP outputs.

Unlike compare_parity.py's discogs-effnet check, this does not use the same 1e-4
tolerance for raw bin values: the C++ side resamples via libsoxr called directly
(SOXR_HQ, matching librosa.load's default res_type) while the Python reference goes
through librosa's own resample path, so tiny time-domain differences are expected
even with matching quality settings. What matters is that the two pipelines land on
the same *embedding* (cosine similarity ~1.0), not bit-identical mel frames.

Usage: uv run python3 compare_dclap_parity.py <py-prefix> <cpp-prefix>
"""
import sys
import numpy as np

EMBED_DIM = 512


def load(prefix, suffix):
    return np.fromfile(f"{prefix}_{suffix}.bin", dtype=np.float32)


def cosine(a, b):
    return float(np.dot(a, b) / (np.linalg.norm(a) * np.linalg.norm(b)))


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <py-prefix> <cpp-prefix>", file=sys.stderr)
        sys.exit(1)
    py_prefix, cpp_prefix = sys.argv[1], sys.argv[2]

    py_emb = load(py_prefix, "embedding")
    cpp_emb = load(cpp_prefix, "embedding")
    print(f"final embedding: py shape={py_emb.shape} cpp shape={cpp_emb.shape}")
    diff = np.abs(py_emb - cpp_emb)
    sim = cosine(py_emb, cpp_emb)
    print(f"final embedding: max_abs_diff={diff.max():.6g}  mean_abs_diff={diff.mean():.6g}  cosine_sim={sim:.8f}")

    py_seg = load(py_prefix, "segment_embeddings").reshape(-1, EMBED_DIM)
    cpp_seg = load(cpp_prefix, "segment_embeddings").reshape(-1, EMBED_DIM)
    print(f"\nper-segment ({py_seg.shape[0]} segments):")
    ok = True
    for i in range(py_seg.shape[0]):
        s = cosine(py_seg[i], cpp_seg[i])
        d = np.abs(py_seg[i] - cpp_seg[i])
        print(f"  segment {i}: cosine_sim={s:.8f}  max_abs_diff={d.max():.6g}  mean_abs_diff={d.mean():.6g}")
        if s < 0.999:
            ok = False

    print()
    if sim > 0.999 and ok:
        print(f"PARITY OK — cosine similarity > 0.999 on final and every per-segment embedding")
        sys.exit(0)
    else:
        print("PARITY FAILED — cosine similarity below 0.999 threshold")
        sys.exit(1)


if __name__ == "__main__":
    main()
