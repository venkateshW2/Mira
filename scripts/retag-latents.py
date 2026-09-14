#!/usr/bin/env python3
"""Refresh the captions on an already-encoded latent set, without re-encoding.

A latent sidecar (``<name>.json`` beside ``<name>.npy``) holds two unrelated things:

  latent bookkeeping  path, relpath, src_relpath, seconds_total, seconds_start,
                      audio_samples, latent_shape, padding_mask
  the caption         trigger, TrackType, genre, instruments, moods, rhythm,
                      dynamics, texture, palette, timing, keywords, bpm, ... , prompt

Re-tagging a folder in mira, or picking a different trigger, changes only the second
group. The audio and the codec have not changed, so the ``.npy`` is bit-identical and
re-running the pre-encoder would spend GPU minutes reproducing a file it already has.
This rewrites the caption half in place and leaves the latents alone.

Use the pre-encoder instead when the AUDIO or the codec changed -- that is the only case
where the latents are actually stale.

    python3 scripts/retag-latents.py --latents sa3-studio/latents/DArkKnight --trigger dkt

Re-emits each source file's mira sidecar at the new trigger, then merges it in. Sources
are located through each sidecar's own ``path`` key, so a moved library is reported rather
than silently half-applied.

Note that a real run rewrites ``<audio>.json`` beside each source file as well as the
latent sidecar -- that is ``mira caption --emit-sidecar`` doing its job, and those files
are regenerable, but it is a write outside --latents and worth knowing about. ``--dry-run``
does not call mira at all, precisely so it cannot.
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Written by the pre-encoder and describing the latent, not the audio's meaning. Anything
# not in this set is caption and gets replaced wholesale -- that is what makes a removed
# tag actually disappear instead of lingering from the previous run.
LATENT_KEYS = {
    "path", "relpath", "src_relpath", "seconds_total", "seconds_start",
    "audio_samples", "latent_shape", "padding_mask",
}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--latents", required=True, help="directory of .npy + .json latent pairs")
    ap.add_argument("--trigger", required=True, help="trigger token to caption with, e.g. dkt")
    ap.add_argument("--mira", default=str(ROOT / "build/src/mira"), help="path to the mira binary")
    ap.add_argument("--db", default=None, help="mira database (default: mira's own default)")
    ap.add_argument("--dry-run", action="store_true", help="report what would change, write nothing")
    args = ap.parse_args()

    latents = Path(args.latents).expanduser().resolve()
    sidecars = sorted(p for p in latents.glob("*.json") if p.stem != "details")
    if not sidecars:
        sys.exit(f"no latent sidecars in {latents}")

    updated = missing = failed = 0
    for sc in sidecars:
        latent = json.loads(sc.read_text())
        src = Path(latent.get("path", ""))
        if not src.is_file():
            print(f"  MISSING SOURCE  {sc.name}  ->  {src}")
            missing += 1
            continue

        # --emit-sidecar WRITES <audio>.json next to the source, so it must not run under
        # --dry-run. (It did in the first version of this script, which made the dry run
        # quietly re-tag every source file -- the exact thing a dry run promises not to do.)
        if args.dry_run:
            if latent.get("trigger") != args.trigger:
                print(f"  would re-caption  {src.name}  ({latent.get('trigger')} -> {args.trigger})")
            updated += 1
            continue

        cmd = [args.mira, "caption", str(src), "--trigger", args.trigger, "--emit-sidecar"]
        if args.db:
            cmd += ["--db", args.db]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(f"  CAPTION FAILED  {src.name}: {r.stderr.strip().splitlines()[:1]}")
            failed += 1
            continue

        fresh = json.loads(src.with_suffix(".json").read_text())
        merged = {k: v for k, v in latent.items() if k in LATENT_KEYS}
        merged.update(fresh)

        if merged == latent:
            continue
        sc.write_text(json.dumps(merged, ensure_ascii=False))
        updated += 1

    verb = "would update" if args.dry_run else "updated"
    print(f"{verb} {updated}/{len(sidecars)} sidecars"
          + (f", {missing} missing source" if missing else "")
          + (f", {failed} failed" if failed else ""))
    # A partially-applied dataset trains on a mix of old and new captions and looks fine
    # while doing it, so treat any gap as a failure to act on rather than a warning.
    return 1 if (missing or failed) else 0


if __name__ == "__main__":
    sys.exit(main())
