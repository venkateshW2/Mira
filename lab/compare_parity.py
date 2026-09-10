#!/usr/bin/env python3
"""Phase 0 day 3 spike (PRD §9): diff Python vs C++ parity outputs.

Usage: uv run python3 compare_parity.py <py-prefix> <cpp-prefix>
Exit criterion from the PRD: embeddings must agree to ~1e-4.
"""
import sys
import numpy as np

PATCH_SIZE, NUMBER_BANDS, EMBED_DIM = 128, 96, 1280


def load(prefix, suffix, cols):
    arr = np.fromfile(f"{prefix}_{suffix}.bin", dtype=np.float32)
    return arr.reshape(-1, cols)


def report(name, a, b):
    if a.shape != b.shape:
        print(f"{name}: SHAPE MISMATCH {a.shape} vs {b.shape}")
        return False
    diff = np.abs(a - b)
    max_abs = diff.max()
    mean_abs = diff.mean()
    # relative, guarding against near-zero reference values
    denom = np.maximum(np.abs(a), 1e-6)
    max_rel = (diff / denom).max()
    print(f"{name}: shape={a.shape}  max_abs={max_abs:.6g}  mean_abs={mean_abs:.6g}  max_rel={max_rel:.6g}")
    return max_abs < 1e-4


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <py-prefix> <cpp-prefix>", file=sys.stderr)
        sys.exit(1)
    py_prefix, cpp_prefix = sys.argv[1], sys.argv[2]

    py_patches = load(py_prefix, "patches", NUMBER_BANDS).reshape(-1, PATCH_SIZE, NUMBER_BANDS)
    cpp_patches = load(cpp_prefix, "patches", NUMBER_BANDS).reshape(-1, PATCH_SIZE, NUMBER_BANDS)
    ok_patches = report("mel patches", py_patches, cpp_patches)

    py_emb = load(py_prefix, "embeddings", EMBED_DIM)
    cpp_emb = load(cpp_prefix, "embeddings", EMBED_DIM)
    ok_emb = report("embeddings ", py_emb, cpp_emb)

    print()
    if ok_patches and ok_emb:
        print("PARITY OK — Python and C++ agree to < 1e-4 (PRD §9 day 3 exit criterion met)")
        sys.exit(0)
    else:
        print("PARITY FAILED")
        sys.exit(1)


if __name__ == "__main__":
    main()
