#!/usr/bin/env python3
"""Phase 4 spike (spike/05_dclap_parity): DCLAP audio-embedding reference.

Reimplements the exact preprocessing from AudioMuse-AI-DCLAP's README
(https://github.com/NeptuneHub/AudioMuse-AI-DCLAP) so a future C++ port
(spike/05_dclap_parity's C++ side) has something to diff against, the same
two-sided-parity discipline as parity_reference.py / compare_parity.py did for
discogs-effnet (PRD §9 day 3).

Preprocessing, read directly from the model author's own README, not guessed:
  - resample to 48kHz mono
  - int16 quantize round-trip (matches the PyTorch CLAP preprocessing the student
    was distilled against -- skipping this would silently shift every embedding)
  - split into 10s segments (480000 samples) at 50% overlap (240000 hop); short
    files are zero-padded to one full segment; the tail segment is captured even
    if it doesn't land on a hop boundary
  - per segment: 128-mel log-power spectrogram (n_fft=2048, hop=480, htk=False/
    slaney-normalized -- librosa's default mel scale, fmin=0, fmax=14000),
    power_to_db with amin=1e-10 and top_db=None (no dynamic-range clipping)
  - run each segment through the student ONNX audio encoder individually (fixed
    batch=1), average the per-segment embeddings, then L2-normalize

Usage:
    uv run python3 dclap_parity_reference.py <audio-file> <out-prefix>

Writes <out-prefix>_segment_embeddings.bin (n_segments*512 float32, pre-average),
<out-prefix>_embedding.bin (512 float32, the final averaged+normalized vector),
and a .shape.txt with n_segments.
"""
import sys
import numpy as np
import librosa
import onnxruntime as ort

SR = 48000
SEGMENT_LENGTH = 480000  # 10s @ 48kHz
HOP_LENGTH = 240000      # 50% overlap
N_MELS = 128
N_FFT = 2048
HOP_LENGTH_MELS = 480
EMBED_DIM = 512

AUDIO_MODEL_PATH = "../models/similarity-embeddings/dclap/model_epoch_36.onnx"


def extract_segmented_logmels(audio_path: str) -> list[np.ndarray]:
    y, _ = librosa.load(audio_path, sr=SR, mono=True)

    # Quantize to int16 and back -- matches the PyTorch CLAP preprocessing the
    # student was distilled against (README is explicit about this step).
    y = np.clip(y, -1.0, 1.0)
    y = (y * 32767.0).astype(np.int16)
    y = (y / 32767.0).astype(np.float32)

    total = len(y)
    segments = []

    if total <= SEGMENT_LENGTH:
        segments.append(np.pad(y, (0, SEGMENT_LENGTH - total)))
    else:
        for start in range(0, total - SEGMENT_LENGTH + 1, HOP_LENGTH):
            segments.append(y[start:start + SEGMENT_LENGTH])
        last_start = len(segments) * HOP_LENGTH
        if last_start < total:
            segments.append(y[-SEGMENT_LENGTH:])

    mel_segments = []
    for segment in segments:
        mel = librosa.feature.melspectrogram(
            y=segment, sr=SR, n_fft=N_FFT, hop_length=HOP_LENGTH_MELS,
            win_length=N_FFT, window='hann', center=True, pad_mode='reflect',
            power=2.0, n_mels=N_MELS, fmin=0, fmax=14000,
        )
        log_mel = librosa.power_to_db(mel, ref=1.0, amin=1e-10, top_db=None)
        mel_segments.append(log_mel[np.newaxis, np.newaxis, :, :].astype(np.float32))

    return mel_segments


def compute_embedding(mel_segments: list[np.ndarray]) -> tuple[np.ndarray, np.ndarray]:
    sess = ort.InferenceSession(AUDIO_MODEL_PATH, providers=["CPUExecutionProvider"])
    input_name = sess.get_inputs()[0].name

    segment_embs = []
    for mel in mel_segments:
        emb = sess.run(None, {input_name: mel})[0]
        segment_embs.append(emb[0])
    segment_embs = np.stack(segment_embs).astype(np.float32)  # [n_segments, 512]

    avg = np.mean(segment_embs, axis=0)
    avg = avg / (np.linalg.norm(avg) + 1e-9)
    return segment_embs, avg.astype(np.float32)


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <audio-file> <out-prefix>", file=sys.stderr)
        sys.exit(1)

    audio_path, out_prefix = sys.argv[1], sys.argv[2]

    mel_segments = extract_segmented_logmels(audio_path)
    print(f"segments: {len(mel_segments)} x {mel_segments[0].shape}")

    segment_embs, avg_emb = compute_embedding(mel_segments)
    print(f"segment embeddings: {segment_embs.shape}")
    print(f"final embedding: {avg_emb.shape}  ||v||={np.linalg.norm(avg_emb):.6f}")

    segment_embs.tofile(f"{out_prefix}_segment_embeddings.bin")
    avg_emb.tofile(f"{out_prefix}_embedding.bin")
    with open(f"{out_prefix}.shape.txt", "w") as f:
        f.write(f"n_segments={segment_embs.shape[0]}\n")

    print(f"wrote {out_prefix}_segment_embeddings.bin, {out_prefix}_embedding.bin")


if __name__ == "__main__":
    main()
