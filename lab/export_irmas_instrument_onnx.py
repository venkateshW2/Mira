"""One-time torch -> ONNX export for nii-yamagishilab/predominant-instrument-recognition's
IRMAS-fine-tuned predominant-instrument recognizer (PRD-style: "build-time only, the
converted .onnx is committed", same precedent as tf2onnx elsewhere in lab/).

Why this model, not mtg_jamendo_instrument, for isolated stems: mtg_jamendo_instrument's
embedding was trained entirely on full mixes, so it reads production/synthesis character
rather than instrument identity on an isolated part (real finding, TASKS.md). This model
was pretrained on isolated NSynth notes then fine-tuned on IRMAS's predominant-instrument
task -- built for exactly the isolated/sparse-audio case mtg_jamendo_instrument fails on.

Architecture (reverse-engineered from the published checkpoint + source, not guessed --
see conversion_sources/irmas_predominant/ for the exact fetched source files and
docs/irmas_instrument_notes.md for the full derivation):
    raw waveform (16kHz, mono, loudness-normalized to -12 LUFS)
    -> SincConv (learnable sinc filterbank, 128 channels, kernel 801, stride 192)
    -> unsqueeze -> BatchNorm2d(1)
    -> ResNet34 (custom 16-64-128 channel variant, not the ImageNet 64-512 one)
    -> AvgPool2d((1,16)) over the 128-wide SincConv-channel axis
    -> LDE pooling (D=8 learnable dictionary components) over the time axis
    -> classifier: ReLU -> BatchNorm1d(1024) -> Dropout(0.15, no-op in eval) -> Linear(1024,11)
    -> 11 raw logits (BCEWithLogitsLoss was used in training -- apply sigmoid for
       per-class probabilities, this graph does NOT include a final sigmoid)

Trained on 1-second clips (write_metadata_irmas.py's slice() -- 3x 1s slices per training
clip), so 1.0s @ 16kHz (16000 samples) is the input length this model actually saw.
Class order (label_dict in write_metadata_irmas.py, NOT alphabetical):
    cel, cla, flu, gac, gel, org, pia, sax, tru, vio, voi
"""

import sys
from pathlib import Path

import torch
import torch.nn as nn
from addict import Dict as AttrDict

sys.path.insert(0, str(Path(__file__).parent / "conversion_sources" / "irmas_predominant"))

from src.models.instr_emd_sinc_model import InstrEmdSincModel  # noqa: E402

CKPT_PATH = Path(__file__).parent / "conversion_sources/irmas_predominant/model_ckpt_tmp/irnet4irmas.ckpt"
OUT_PATH = Path(__file__).parent.parent / "models/classification-heads/irmas-predominant-instrument/irmas-predominant-instrument-1.onnx"

# Matches conversion_sources/irmas_predominant/irnet.py exactly (the base architecture
# config) -- num_classes is irrelevant here since fc0/bn0/fc11 get replaced with Identity
# below, same as the real IRMASRecognizer wrapper does.
OPT = AttrDict(dict(
    transform=dict(
        type="SincConv", sr=16000, out_channels=128, kernel_size=50, stride=12,
        in_channels=1, padding="same", init_type="mel", min_low_hz=5, min_band_hz=5,
        requires_grad=True,
    ),
    backbone=dict(type="resnet34", pretrained=""),
    spec_bn="hi",
    neck=dict(type="LDE", D=8, pooling="mean", network_type="lde", distance_type="sqr"),
    head1=dict(type="LinearClsHead", num_classes=11, hidden_dim=512),
))


class IRMASExport(nn.Module):
    """Mirrors src/lms/irmas_mie.py's IRMASRecognizer with csf='mlp' (the checkpoint's
    own hparams.csf) -- fc0/bn0/fc11 become Identity, a fresh classifier head replaces
    them. Reconstructed directly (not importing irmas_mie.py) since that file also pulls
    in a training-only dependency graph (IRMASDataset, optimization schedulers) irrelevant
    to inference/export.
    """

    def __init__(self, opt):
        super().__init__()
        self.feature_extractor = InstrEmdSincModel(opt)
        feat_dim = self.feature_extractor.fc0.in_features  # 1024 = 128 * D(8)
        self.feature_extractor.fc0 = nn.Identity()
        self.feature_extractor.bn0 = nn.Identity()
        self.feature_extractor.fc11 = nn.Identity()
        self.classifier = nn.Sequential(
            nn.ReLU(),
            nn.BatchNorm1d(feat_dim),
            nn.Dropout(p=0.15),
            nn.Linear(feat_dim, 11),
        )

    def forward(self, x):
        emb = self.feature_extractor(x)[0]
        logits = self.classifier(emb)
        return logits


def main():
    model = IRMASExport(OPT)
    ckpt = torch.load(CKPT_PATH, map_location="cpu", weights_only=False)
    state_dict = ckpt["state_dict"]

    missing, unexpected = model.load_state_dict(state_dict, strict=True)
    print(f"loaded state_dict: missing={missing} unexpected={unexpected}")
    model.eval()

    # Sanity forward pass: 1 second @ 16kHz, matching the training clip length.
    dummy = torch.randn(1, 16000)
    with torch.no_grad():
        out = model(dummy)
    print("output shape:", out.shape, "(expect [1, 11])")
    assert out.shape == (1, 11)

    OUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    # Static input shape (1, 16000) deliberately, not dynamic -- the model was trained on
    # exactly 1-second clips (write_metadata_irmas.py's slice() logic), and the C++ side
    # will always feed fixed 1s windows sliced from longer audio (same pattern as
    # ContentGate.cpp's chunking), so there's no real use for a dynamic time axis here,
    # and SincConv's "same"-padding arithmetic is plain Python int math on x.shape --
    # safer to bake in the one shape actually used than trust the trace to generalize it.
    torch.onnx.export(
        model,
        dummy,
        str(OUT_PATH),
        input_names=["waveform"],
        output_names=["logits"],
        opset_version=17,
        dynamo=False,  # torch 2.14's default dynamo-based exporter needs onnxscript;
                        # the older TorchScript-tracing exporter doesn't and is more
                        # predictable for this custom (non-torchvision) architecture.
    )
    print(f"wrote {OUT_PATH} ({OUT_PATH.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
