"""Exports roberta-base's byte-level BPE tables as plain text for mira's C++ tokenizer.

The CLAP text tower (clap_text_model.onnx) takes input_ids/attention_mask, not text, so
mira needs the tokenizer itself. HuggingFace ships it as one tokenizer.json; parsing that
in C++ would mean a JSON dependency the CLI doesn't have, so the tables are flattened here
to two plain-text files instead -- the same "do the conversion offline in lab/" approach
export_irmas_instrument_onnx.py already uses.

Writes into models/similarity-embeddings/dclap/:
  roberta-vocab.tsv   <token>\t<id>, one per line, token is byte-level-encoded already
  roberta-merges.txt  <a> <b>, one merge per line, in rank order
  roberta-parity.tsv  <text>\t<id,id,...>  -- fixture the C++ tokenizer is checked against
"""
import json, sys
from pathlib import Path
from huggingface_hub import hf_hub_download
from tokenizers import Tokenizer

out = Path("models/similarity-embeddings/dclap")
tj = json.loads(Path(hf_hub_download("roberta-base", "tokenizer.json")).read_text())
vocab, merges = tj["model"]["vocab"], tj["model"]["merges"]

with (out / "roberta-vocab.tsv").open("w", encoding="utf-8") as f:
    for tok, idx in sorted(vocab.items(), key=lambda kv: kv[1]):
        assert "\t" not in tok and "\n" not in tok, repr(tok)
        f.write(f"{tok}\t{idx}\n")

with (out / "roberta-merges.txt").open("w", encoding="utf-8") as f:
    for m in merges:
        f.write((m if isinstance(m, str) else " ".join(m)) + "\n")

# Parity fixture: real queries plus the punctuation/casing/unicode edge cases a
# hand-rolled byte-level BPE is most likely to get wrong.
tok = Tokenizer.from_file(hf_hub_download("roberta-base", "tokenizer.json"))
cases = [
    "This is a sound of snare drum.", "This is a sound of a singing voice.",
    "brass", "strings", "bass guitar", "solo violin", "dark brass swell",
    "  leading and trailing  ", "Mixed CASE Words", "hyphen-ated and apostrophe's",
    "numbers 140 bpm 808", "unicode: café naïve — dash", "emoji 🎺 trumpet",
    "", "a", "the sound of rain falling on a tin roof at night",
]
with (out / "roberta-parity.tsv").open("w", encoding="utf-8") as f:
    for c in cases:
        ids = tok.encode(c).ids
        f.write(c.replace("\t", " ") + "\t" + ",".join(map(str, ids)) + "\n")

print(f"vocab {len(vocab)}  merges {len(merges)}  parity cases {len(cases)}")
