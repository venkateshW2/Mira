# 06_embedded_python — PASSED (2026-09-13)

Proves: **a standalone CPython + MLX can be embedded in a signed `.app`, run Metal GPU
compute as a child process, and keep a valid code signature** — the one unknown blocking
[PACKAGING.md](../../sa3-studio/PACKAGING.md) option B (embed Python rather than port SA3
inference to C++).

```bash
./build.sh          # assemble, sign (hardened runtime), verify, run, re-verify
./build.sh --lax    # fallback: also disables library validation (NOT needed — see below)
```

## Result

| Question | Answer |
|---|---|
| Does a bundled interpreter run under hardened runtime? | **Yes** |
| Does MLX reach the GPU from inside the bundle? | **Yes** — `Device(gpu, 0)` |
| Does it need `com.apple.security.cs.allow-jit`? | **No** — runs with *empty* entitlements |
| Does it need library validation disabled? | **No** — same-identity signing satisfies it |
| Does `codesign --verify --deep --strict` pass? | **Yes** |
| Does it still pass *after being run*? | **Yes, but only with the fix below** |
| Does it work when moved to another path? | **Yes** — signature and GPU both survive |
| Bundle size | **235 MB** (56 MB Python + 181 MB MLX/numpy) |

Signed `flags=0x10002(adhoc,runtime)` — hardened runtime genuinely on, not skipped.

## The trap this spike found

**A bundled interpreter writes `__pycache__/*.pyc` into `Contents/Resources` on first run,
which breaks the bundle's code signature.** First run works; `codesign --verify` then fails
with *"a sealed resource is missing or invalid"*, and a notarised app would start being
rejected by Gatekeeper after it had already been shipped and run once.

24 `.pyc` files were enough to break the seal.

Fix: `PYTHONDONTWRITEBYTECODE=1` in the child's environment (`host.cpp`). Step 5 of
`build.sh` is a regression guard — it counts `.pyc` files after running and re-verifies the
signature, so this cannot silently come back.

## What is proven vs what is not

**Proven:** everything above, using **ad-hoc signing** (`codesign -s -`). Ad-hoc exercises
the same hardened-runtime and library-validation rules as a real certificate, which is where
the technical risk lived.

**Not proven — needs an Apple Developer account ($99/yr):** `security find-identity` reports
**0 valid identities** on this machine, so no Developer ID certificate exists yet. That means
the final submission step is untested:

- `notarytool submit` + `stapler staple`
- Gatekeeper acceptance of a downloaded (quarantined) copy

These are a submission formality once signing is clean, not a design risk — but they are
genuinely untested, and an Apple Developer account is a prerequisite for shipping to anyone
else.

## Structure

| File | Role |
|---|---|
| `host.cpp` | stand-in for mira's JUCE `ChildProcess` call site; spawns the bundled interpreter |
| `worker.py` | stand-in for `sa3_mlx.py`; does real MLX GPU work and reports back as JSON |
| `build.sh` | assemble → sign inside-out → verify → run → re-verify |
| `entitlements.plist` | `allow-jit` only. Kept for reference; measured to be unnecessary |
| `pyruntime/`, `pysite/`, `build/` | generated, gitignored |

`pyruntime/` comes from `uv python install --install-dir`, which ships
python-build-standalone — the same distribution PACKAGING.md proposes embedding.

## Next

The real inference worker is not this `worker.py`. PACKAGING.md §B calls for a **persistent**
worker that loads the model once (the measured DiT load is 44 s) and then accepts JSON
commands on stdin, so generation cost drops from 44 s per click to ~10 s.
