#!/usr/bin/env bash
# Smoke test for the mira CLI. Builds the project, then runs `mira scan` against
# fixtures/ and asserts the database ends up in the state it should. Run this after
# any change to src/ — no UI exists yet (Phases 1-4 are CLI-only, PRD §8), so this
# script plus `sqlite3 <db>` by hand are the way to check the build actually works.
#
# Usage: tests/smoke_test.sh
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT/build"
MIRA="$BUILD_DIR/src/mira"
TESTDB="$(mktemp -t mira_smoke_XXXXXX).db"

PASS=0
FAIL=0

pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1"; }

assert_contains() {
    local desc="$1" haystack="$2" needle="$3"
    if [[ "$haystack" == *"$needle"* ]]; then pass "$desc"
    else fail "$desc — expected to find '$needle' in:"$'\n'"$haystack"; fi
}

assert_eq() {
    local desc="$1" expected="$2" actual="$3"
    if [[ "$expected" == "$actual" ]]; then pass "$desc"
    else fail "$desc — expected '$expected', got '$actual'"; fi
}

cleanup() { rm -f "$TESTDB" "$TESTDB-wal" "$TESTDB-shm"; }
trap cleanup EXIT

# macOS has no `timeout` builtin. Runs "$@", killing it after $1 seconds if still alive.
run_with_timeout() {
    local secs="$1"; shift
    "$@" & local pid=$!
    ( sleep "$secs" && kill -9 "$pid" 2>/dev/null ) & local watchdog=$!
    wait "$pid" 2>/dev/null; local code=$?
    kill "$watchdog" 2>/dev/null
    return $code
}

echo "== building mira =="
if ! cmake --build "$BUILD_DIR" --target mira 2>&1 | tail -20; then
    echo "BUILD FAILED — fix compile errors before running the smoke test"
    exit 1
fi

if [[ ! -x "$MIRA" ]]; then
    echo "FAIL: $MIRA does not exist after build"
    exit 1
fi

echo
echo "== test: --help exits cleanly =="
OUT=$("$MIRA" --help 2>&1)
CODE=$?
assert_eq "exit code" "0" "$CODE"
assert_contains "usage text mentions scan" "$OUT" "mira scan"

echo
echo "== test: no arguments is an error =="
"$MIRA" >/dev/null 2>&1
CODE=$?
if [[ "$CODE" != "0" ]]; then pass "exit code non-zero ($CODE)"; else fail "expected non-zero exit code, got 0"; fi

echo
echo "== test: scan requires at least one directory =="
"$MIRA" scan --db "$TESTDB" >/dev/null 2>&1
CODE=$?
if [[ "$CODE" != "0" ]]; then pass "exit code non-zero ($CODE)"; else fail "expected non-zero exit code, got 0"; fi

echo
echo "== test: fresh scan of fixtures/ =="
OUT=$("$MIRA" scan "$ROOT/fixtures" --db "$TESTDB" 2>&1)
CODE=$?
assert_eq "exit code" "0" "$CODE"
assert_contains "reports 1 new audio file" "$OUT" "1 new"
assert_contains "reports 1 file skipped (README.md)" "$OUT" "1 non-audio files skipped"

ROW=$(sqlite3 "$TESTDB" "SELECT sha256, content_type, content_type_source, machine, human FROM files WHERE path LIKE '%flamenco.wav'")
IFS='|' read -r SHA256 CONTENT_TYPE CT_SOURCE MACHINE HUMAN <<< "$ROW"
assert_eq "sha256 is 64 hex chars" "64" "${#SHA256}"
assert_eq "content_type defaults to unknown (scan doesn't analyze)" "unknown" "$CONTENT_TYPE"
assert_eq "content_type_source defaults to router" "router" "$CT_SOURCE"
assert_eq "machine JSON defaults empty" "{}" "$MACHINE"
assert_eq "human JSON defaults empty" "{}" "$HUMAN"

echo
echo "== test: re-scan reports unchanged, not new =="
OUT=$("$MIRA" scan "$ROOT/fixtures" --db "$TESTDB" 2>&1)
assert_contains "reports 1 unchanged" "$OUT" "1 unchanged"
assert_contains "reports 0 new" "$OUT" "0 new"

echo
echo "== test: touching the file's mtime triggers an update, same content =="
touch "$ROOT/fixtures/flamenco.wav"
OUT=$("$MIRA" scan "$ROOT/fixtures" --db "$TESTDB" 2>&1)
assert_contains "reports 1 updated" "$OUT" "1 updated"
SHA256_AFTER=$(sqlite3 "$TESTDB" "SELECT sha256 FROM files WHERE path LIKE '%flamenco.wav'")
assert_eq "sha256 unchanged (content didn't actually change)" "$SHA256" "$SHA256_AFTER"

echo
echo "== test: scanning a nonexistent directory doesn't crash =="
OUT=$("$MIRA" scan "$ROOT/no-such-directory-xyz" --db "$TESTDB" 2>&1)
CODE=$?
assert_eq "exit code" "0" "$CODE"
assert_contains "reports 0 files scanned" "$OUT" "scanned 0 audio files"

echo
echo "== test: duplicate content gets the same sha256 =="
# Own throwaway db and dir — otherwise its row for copy.wav outlives the rm -rf below and
# a later `mira analyze` in this script would try to decode a file that no longer exists.
DUPTESTDB="$(mktemp -t mira_smoke_dup_XXXXXX).db"
DUP_DIR=$(mktemp -d)
cp "$ROOT/fixtures/flamenco.wav" "$DUP_DIR/copy.wav"
"$MIRA" scan "$DUP_DIR" --db "$DUPTESTDB" >/dev/null 2>&1
SHA256_COPY=$(sqlite3 "$DUPTESTDB" "SELECT sha256 FROM files WHERE path LIKE '%copy.wav'")
assert_eq "identical content hashes identically" "$SHA256" "$SHA256_COPY"
rm -rf "$DUP_DIR" "$DUPTESTDB" "$DUPTESTDB-wal" "$DUPTESTDB-shm"

echo
echo "== test: macOS AppleDouble sidecars (._Foo.wav) are not indexed as audio =="
# Found on a real exFAT stem-delivery drive: every real .wav had a same-named ._.wav
# resource-fork sidecar sitting next to it, which passed the extension check and got
# scanned as if it were audio, only to fail to decode later at analyze time.
APPLEDOUBLE_DIR=$(mktemp -d)
cp "$ROOT/fixtures/flamenco.wav" "$APPLEDOUBLE_DIR/real.wav"
printf '\x00\x05\x16\x07\x00\x02\x00\x00' > "$APPLEDOUBLE_DIR/._real.wav" # fake AppleDouble header
APPLEDOUBLE_DB="$(mktemp -t mira_smoke_appledouble_XXXXXX).db"
OUT=$("$MIRA" scan "$APPLEDOUBLE_DIR" --db "$APPLEDOUBLE_DB" 2>&1)
assert_contains "reports 1 real audio file" "$OUT" "1 new"
assert_contains "reports the sidecar as skipped, not scanned" "$OUT" "1 macOS AppleDouble sidecars skipped"
FILE_COUNT=$(sqlite3 "$APPLEDOUBLE_DB" "SELECT COUNT(*) FROM files")
assert_eq "only the real file made it into the database" "1" "$FILE_COUNT"
rm -rf "$APPLEDOUBLE_DIR"
rm -f "$APPLEDOUBLE_DB" "$APPLEDOUBLE_DB-wal" "$APPLEDOUBLE_DB-shm"

echo
echo "== test: analyze routes a file by duration (flamenco.wav -> loop, 14.2s) =="
"$MIRA" scan "$ROOT/fixtures" --db "$TESTDB" >/dev/null 2>&1
# --chords --transcribe --recheck-tempo: chords/transcription/essentia-tempo-recheck are
# all opt-in now (TASKS.md), but this $TESTDB/$MACHINE is reused by the key/chords/notes
# tests immediately below, which need essentia's rhythm fields present too.
OUT=$("$MIRA" analyze --db "$TESTDB" --chords --transcribe --recheck-tempo 2>&1)
assert_contains "reports 1 loop routed" "$OUT" "1 loop"
CONTENT_TYPE=$(sqlite3 "$TESTDB" "SELECT content_type FROM files WHERE path LIKE '%flamenco.wav'")
assert_eq "content_type is loop" "loop" "$CONTENT_TYPE"
CT_SOURCE=$(sqlite3 "$TESTDB" "SELECT content_type_source FROM files WHERE path LIKE '%flamenco.wav'")
assert_eq "content_type_source is router" "router" "$CT_SOURCE"
MACHINE=$(sqlite3 "$TESTDB" "SELECT machine FROM files WHERE path LIKE '%flamenco.wav'")
assert_contains "machine JSON has duration_seconds" "$MACHINE" "duration_seconds"

echo
echo "== test: MIR runs for a loop and both tempo estimators produce a plausible BPM =="
assert_contains "machine JSON has a rhythm section" "$MACHINE" "\"rhythm\""
ESSENTIA_BPM=$(echo "$MACHINE" | python3 -c 'import json,sys; print(json.load(sys.stdin)["rhythm"]["essentia_bpm"])')
BEAT_THIS_BPM=$(echo "$MACHINE" | python3 -c 'import json,sys; print(json.load(sys.stdin)["rhythm"]["beat_this_bpm"])')
ESSENTIA_BPM_OK=$(awk -v b="$ESSENTIA_BPM" 'BEGIN{print (b>40 && b<220) ? "yes" : "no"}')
BEAT_THIS_BPM_OK=$(awk -v b="$BEAT_THIS_BPM" 'BEGIN{print (b>40 && b<220) ? "yes" : "no"}')
assert_eq "essentia_bpm is in a plausible range" "yes" "$ESSENTIA_BPM_OK"
assert_eq "beat_this_bpm is in a plausible range" "yes" "$BEAT_THIS_BPM_OK"
BEAT_TICK_COUNT=$(echo "$MACHINE" | python3 -c 'import json,sys; print(len(json.load(sys.stdin)["rhythm"]["essentia_beat_ticks"]))')
assert_eq "full essentia beat array stored, not just the BPM scalar" "yes" \
    "$(awk -v n="$BEAT_TICK_COUNT" 'BEGIN{print (n>5) ? "yes" : "no"}')"

echo
echo "== test: key detection runs on tonal content and is gated off for noise =="
assert_contains "flamenco.wav (tonal) got a key section" "$MACHINE" "\"key\":{"
KEY_NAME=$(echo "$MACHINE" | python3 -c 'import json,sys; print(json.load(sys.stdin)["key"]["key"])')
if [[ -n "$KEY_NAME" && "$KEY_NAME" != "silence" ]]; then
    pass "key name is non-empty and not silence ($KEY_NAME)"
else
    fail "expected a real key name, got '$KEY_NAME'"
fi
CAMELOT=$(echo "$MACHINE" | python3 -c 'import json,sys; print(json.load(sys.stdin)["key"]["camelot"])')
assert_contains "camelot notation looks like N(A|B)" "1A 1B 2A 2B 3A 3B 4A 4B 5A 5B 6A 6B 7A 7B 8A 8B 9A 9B 10A 10B 11A 11B 12A 12B" "$CAMELOT"

if command -v ffmpeg >/dev/null 2>&1; then
    NOISE_DIR=$(mktemp -d)
    ffmpeg -y -loglevel error -f lavfi -i "anoisesrc=color=white:sample_rate=44100:duration=5" "$NOISE_DIR/noise.wav"
    NOISE_DB="$(mktemp -t mira_smoke_noise_XXXXXX).db"
    "$MIRA" scan "$NOISE_DIR" --db "$NOISE_DB" >/dev/null 2>&1
    "$MIRA" analyze --db "$NOISE_DB" --chords >/dev/null 2>&1
    MACHINE_NOISE=$(sqlite3 "$NOISE_DB" "SELECT machine FROM files")
    if [[ "$MACHINE_NOISE" == *"\"key\":{"* ]]; then
        fail "white noise should NOT get a key section (harmonic-content gate), but does"
    else
        pass "white noise correctly gated out of key detection"
    fi
    if [[ "$MACHINE_NOISE" == *"\"chords\":["* ]]; then
        fail "white noise should NOT get a chords section (harmonic-content gate), but does"
    else
        pass "white noise correctly gated out of chord detection"
    fi
    rm -rf "$NOISE_DIR"
    rm -f "$NOISE_DB" "$NOISE_DB-wal" "$NOISE_DB-shm"
else
    echo "  SKIP: ffmpeg not found, skipping key/chord-detection gating test"
fi

echo
echo "== test: chord detection (Chordino) runs alongside key on tonal content =="
assert_contains "machine JSON has a chords array" "$MACHINE" "\"chords\":["
CHORD_COUNT=$(echo "$MACHINE" | python3 -c 'import json,sys; print(len(json.load(sys.stdin)["chords"]))')
assert_eq "found a plausible number of chord segments (>3)" "yes" \
    "$(awk -v n="$CHORD_COUNT" 'BEGIN{print (n>3) ? "yes" : "no"}')"

echo
echo "== test: note transcription (Basic Pitch) runs on a loop, unlike key/chords not gated by flatness =="
assert_contains "machine JSON has a notes array" "$MACHINE" "\"notes\":["
NOTE_COUNT=$(echo "$MACHINE" | python3 -c 'import json,sys; print(len(json.load(sys.stdin)["notes"]))')
assert_eq "found a plausible number of notes on a 14s guitar piece (>10)" "yes" \
    "$(awk -v n="$NOTE_COUNT" 'BEGIN{print (n>10) ? "yes" : "no"}')"
FIRST_PITCH=$(echo "$MACHINE" | python3 -c 'import json,sys; print(json.load(sys.stdin)["notes"][0]["pitch"])')
assert_eq "first note's MIDI pitch is in playable piano range (21-108)" "yes" \
    "$(awk -v p="$FIRST_PITCH" 'BEGIN{print (p>=21 && p<=108) ? "yes" : "no"}')"

if command -v ffmpeg >/dev/null 2>&1; then
    TRANSCRIBE_NOISE_DIR=$(mktemp -d)
    ffmpeg -y -loglevel error -f lavfi -i "anoisesrc=color=white:sample_rate=44100:duration=5" \
        "$TRANSCRIBE_NOISE_DIR/noise.wav"
    TRANSCRIBE_NOISE_DB="$(mktemp -t mira_smoke_transcribe_noise_XXXXXX).db"
    "$MIRA" scan "$TRANSCRIBE_NOISE_DIR" --db "$TRANSCRIBE_NOISE_DB" >/dev/null 2>&1
    "$MIRA" analyze --db "$TRANSCRIBE_NOISE_DB" --transcribe >/dev/null 2>&1
    MACHINE_TRANSCRIBE_NOISE=$(sqlite3 "$TRANSCRIBE_NOISE_DB" "SELECT machine FROM files")
    assert_contains "notes still run on noise (unlike key/chords, no flatness gate)" \
        "$MACHINE_TRANSCRIBE_NOISE" "\"notes\":["
    rm -rf "$TRANSCRIBE_NOISE_DIR"
    rm -f "$TRANSCRIBE_NOISE_DB" "$TRANSCRIBE_NOISE_DB-wal" "$TRANSCRIBE_NOISE_DB-shm"
else
    echo "  SKIP: ffmpeg not found, skipping transcription-not-gated-by-flatness test"
fi

echo
echo "== test: transcription of a file with zero notes doesn't hang (regression) =="
# Found by this suite: drop_overlapping_pitch_bends did `for (size_t i = 0; i < note_events.size() - 1; ...)`
# — size_t underflow to SIZE_MAX when note_events is empty, turning "no notes" (the
# common case on truly silent material) into a multi-billion-iteration hang. Random white
# noise sometimes finds zero notes and sometimes finds one or two, so the bug didn't fire
# every run — pure digital silence should reliably produce zero, pinning it deterministically.
if command -v ffmpeg >/dev/null 2>&1; then
    SILENT_DIR=$(mktemp -d)
    ffmpeg -y -loglevel error -f lavfi -i "anullsrc=r=44100:cl=mono:d=5" "$SILENT_DIR/silence.wav"
    SILENT_DB="$(mktemp -t mira_smoke_silentnotes_XXXXXX).db"
    "$MIRA" scan "$SILENT_DIR" --db "$SILENT_DB" >/dev/null 2>&1
    # --transcribe: transcription is opt-in now (TASKS.md) — without this flag the buggy
    # code path never runs at all, and this regression test would pass trivially.
    if run_with_timeout 15 "$MIRA" analyze --db "$SILENT_DB" --transcribe >/dev/null 2>&1; then
        pass "analyze finished within 15s on a silent (zero-note) file"
    else
        fail "analyze did not finish within 15s on a silent file — the size_t underflow hang is back"
    fi
    rm -rf "$SILENT_DIR"
    rm -f "$SILENT_DB" "$SILENT_DB-wal" "$SILENT_DB-shm"
else
    echo "  SKIP: ffmpeg not found, skipping zero-notes hang regression test"
fi

echo
echo "== test: MIR is skipped for one-shots (tempo on a 0.5s clip is meaningless) =="
if command -v ffmpeg >/dev/null 2>&1; then
    ONESHOT_DIR=$(mktemp -d)
    ffmpeg -y -loglevel error -f lavfi -i "sine=frequency=1000:sample_rate=44100:duration=0.5" "$ONESHOT_DIR/click.wav"
    ONESHOT_DB="$(mktemp -t mira_smoke_oneshot_XXXXXX).db"
    "$MIRA" scan "$ONESHOT_DIR" --db "$ONESHOT_DB" >/dev/null 2>&1
    "$MIRA" analyze --db "$ONESHOT_DB" >/dev/null 2>&1
    CONTENT_TYPE=$(sqlite3 "$ONESHOT_DB" "SELECT content_type FROM files")
    MACHINE_ONESHOT=$(sqlite3 "$ONESHOT_DB" "SELECT machine FROM files")
    assert_eq "0.5s clip routed as one_shot" "one_shot" "$CONTENT_TYPE"
    if [[ "$MACHINE_ONESHOT" == *"\"rhythm\""* ]]; then
        fail "one-shot should NOT have a rhythm section, but does"
    else
        pass "one-shot has no rhythm section"
    fi
    rm -rf "$ONESHOT_DIR"
    rm -f "$ONESHOT_DB" "$ONESHOT_DB-wal" "$ONESHOT_DB-shm"
else
    echo "  SKIP: ffmpeg not found, skipping one-shot MIR-gating test"
fi

echo
echo "== test: analyze skips already-routed files without --force =="
OUT=$("$MIRA" analyze --db "$TESTDB" 2>&1)
assert_contains "reports nothing to analyze" "$OUT" "nothing to analyze"

echo
echo "== test: --force re-routes =="
OUT=$("$MIRA" analyze --db "$TESTDB" --force 2>&1)
assert_contains "reports 1 loop routed again" "$OUT" "1 loop"

echo
echo "== test: analyze doesn't crash on a row whose file no longer exists on disk =="
GONE_DIR=$(mktemp -d)
cp "$ROOT/fixtures/flamenco.wav" "$GONE_DIR/gone.wav"
GONE_DB="$(mktemp -t mira_smoke_gone_XXXXXX).db"
"$MIRA" scan "$GONE_DIR" --db "$GONE_DB" >/dev/null 2>&1
rm -f "$GONE_DIR/gone.wav" # row still in the db, file is not on disk
OUT=$("$MIRA" analyze --db "$GONE_DB" 2>&1)
CODE=$?
assert_eq "exit code is still 0 (one bad file doesn't fail the whole run)" "0" "$CODE"
assert_contains "reports the file could not be decoded" "$OUT" "could not be decoded"
rm -rf "$GONE_DIR" "$GONE_DB" "$GONE_DB-wal" "$GONE_DB-shm"

echo
echo "== test: sibling-set stem detection (two same-length files, same folder) =="
rm -f "$TESTDB" "$TESTDB-wal" "$TESTDB-shm"
SIBLING_DIR=$(mktemp -d)
cp "$ROOT/fixtures/flamenco.wav" "$SIBLING_DIR/strings.wav"
cp "$ROOT/fixtures/flamenco.wav" "$SIBLING_DIR/rhythm.wav"
"$MIRA" scan "$SIBLING_DIR" --db "$TESTDB" >/dev/null 2>&1
"$MIRA" analyze --db "$TESTDB" >/dev/null 2>&1
TYPES=$(sqlite3 "$TESTDB" "SELECT content_type FROM files ORDER BY path")
assert_eq "both siblings routed as stem" "stem"$'\n'"stem" "$TYPES"
GROUP_IDS=$(sqlite3 "$TESTDB" "SELECT DISTINCT group_id FROM files")
assert_eq "both siblings share one group_id" "1" "$(echo "$GROUP_IDS" | wc -l | tr -d ' ')"
rm -rf "$SIBLING_DIR"

echo
echo "== test: filename/folder-pattern stem detection (PRD §12.3 route 1) =="
rm -f "$TESTDB" "$TESTDB-wal" "$TESTDB-shm"
PATTERN_DIR=$(mktemp -d)
mkdir -p "$PATTERN_DIR/STEMS"
cp "$ROOT/fixtures/flamenco.wav" "$PATTERN_DIR/STEMS/solo.wav"
"$MIRA" scan "$PATTERN_DIR" --db "$TESTDB" >/dev/null 2>&1
"$MIRA" analyze --db "$TESTDB" >/dev/null 2>&1
CONTENT_TYPE=$(sqlite3 "$TESTDB" "SELECT content_type FROM files")
CT_SOURCE=$(sqlite3 "$TESTDB" "SELECT content_type_source FROM files")
assert_eq "a file inside a STEMS/ folder is routed as stem" "stem" "$CONTENT_TYPE"
assert_eq "content_type_source stays router, not declared (only --as stem declares)" "router" "$CT_SOURCE"
rm -rf "$PATTERN_DIR"

echo
echo "== test: mira analyze --limit caps how many files get processed =="
rm -f "$TESTDB" "$TESTDB-wal" "$TESTDB-shm"
LIMIT_DIR=$(mktemp -d)
cp "$ROOT/fixtures/flamenco.wav" "$LIMIT_DIR/a.wav"
cp "$ROOT/fixtures/flamenco.wav" "$LIMIT_DIR/b.wav"
"$MIRA" scan "$LIMIT_DIR" --db "$TESTDB" >/dev/null 2>&1
"$MIRA" analyze --db "$TESTDB" --limit 1 >/dev/null 2>&1
ANALYZED_COUNT=$(sqlite3 "$TESTDB" "SELECT COUNT(*) FROM files WHERE analyzed_at IS NOT NULL")
assert_eq "--limit 1 analyzed exactly 1 of 2 files" "1" "$ANALYZED_COUNT"
rm -rf "$LIMIT_DIR"

echo
echo "== test: mira analyze --content-type requires --force =="
OUT_CT=$("$MIRA" analyze --db "$TESTDB" --content-type loop 2>&1)
CODE_CT=$?
if [[ "$CODE_CT" != "0" ]]; then pass "exit code non-zero without --force ($CODE_CT)"
else fail "expected non-zero exit code for --content-type without --force, got 0"; fi
assert_contains "explains why" "$OUT_CT" "only has an effect with --force"

echo
echo "== test: --as stem declaration overrides content_type, but analyze still runs on it =="
rm -f "$TESTDB" "$TESTDB-wal" "$TESTDB-shm"
"$MIRA" scan "$ROOT/fixtures" --db "$TESTDB" --as stem >/dev/null 2>&1
CONTENT_TYPE=$(sqlite3 "$TESTDB" "SELECT content_type FROM files WHERE path LIKE '%flamenco.wav'")
CT_SOURCE=$(sqlite3 "$TESTDB" "SELECT content_type_source FROM files WHERE path LIKE '%flamenco.wav'")
assert_eq "content_type is stem immediately after scan" "stem" "$CONTENT_TYPE"
assert_eq "content_type_source is declared" "declared" "$CT_SOURCE"
OUT=$("$MIRA" analyze --db "$TESTDB" 2>&1)
# Declared stems are NOT re-routed, but they still need active-region detection etc
# (PRD §5: it runs for stems "regardless of duration") — declaration only skips routing.
assert_contains "analyze processes the declared stem (not skipped)" "$OUT" "1 stem"
CONTENT_TYPE_AFTER=$(sqlite3 "$TESTDB" "SELECT content_type FROM files WHERE path LIKE '%flamenco.wav'")
CT_SOURCE_AFTER=$(sqlite3 "$TESTDB" "SELECT content_type_source FROM files WHERE path LIKE '%flamenco.wav'")
assert_eq "content_type still stem after analyze" "stem" "$CONTENT_TYPE_AFTER"
assert_eq "content_type_source still declared (never overwritten to router)" "declared" "$CT_SOURCE_AFTER"

echo
echo "== test: active-region detection runs for a stem, not for an ordinary short loop =="
ACTIVE_RATIO_STEM=$(sqlite3 "$TESTDB" "SELECT active_ratio FROM files WHERE path LIKE '%flamenco.wav'")
assert_eq "declared stem got an active_ratio (always runs for stems, PRD §5)" "1.0" "$ACTIVE_RATIO_STEM"

rm -f "$TESTDB" "$TESTDB-wal" "$TESTDB-shm"
"$MIRA" scan "$ROOT/fixtures" --db "$TESTDB" >/dev/null 2>&1
"$MIRA" analyze --db "$TESTDB" >/dev/null 2>&1
ACTIVE_RATIO_LOOP=$(sqlite3 "$TESTDB" "SELECT active_ratio FROM files WHERE path LIKE '%flamenco.wav'")
assert_eq "ordinary 14s loop got no active_ratio (not a stem, well under 5 min)" "" "$ACTIVE_RATIO_LOOP"

echo
echo "== test: active-region detection finds real silence gaps =="
if command -v ffmpeg >/dev/null 2>&1; then
    SILENCE_DIR=$(mktemp -d)
    # 2s silence, 3s tone, 2s silence, 3s tone, 2s silence = 12s, ~50% active
    ffmpeg -y -loglevel error \
        -f lavfi -i "anullsrc=r=44100:cl=mono:d=2" \
        -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=3" \
        -f lavfi -i "anullsrc=r=44100:cl=mono:d=2" \
        -f lavfi -i "sine=frequency=880:sample_rate=44100:duration=3" \
        -f lavfi -i "anullsrc=r=44100:cl=mono:d=2" \
        -filter_complex "[0][1][2][3][4]concat=n=5:v=0:a=1[out]" -map "[out]" \
        "$SILENCE_DIR/silence_test.wav"
    rm -f "$TESTDB" "$TESTDB-wal" "$TESTDB-shm"
    "$MIRA" scan "$SILENCE_DIR" --db "$TESTDB" --as stem >/dev/null 2>&1
    "$MIRA" analyze --db "$TESTDB" >/dev/null 2>&1
    SPAN_COUNT=$(sqlite3 "$TESTDB" "SELECT json_array_length(active_spans) FROM files")
    assert_eq "found 2 active spans (two tone segments)" "2" "$SPAN_COUNT"
    ACTIVE_RATIO=$(sqlite3 "$TESTDB" "SELECT active_ratio FROM files")
    # ~6s active out of 12s = 0.5, allow frame-granularity slop
    IN_RANGE=$(awk -v r="$ACTIVE_RATIO" 'BEGIN{print (r>0.4 && r<0.6) ? "yes" : "no"}')
    assert_eq "active_ratio is roughly 0.5" "yes" "$IN_RANGE"
    rm -rf "$SILENCE_DIR"
else
    echo "  SKIP: ffmpeg not found, skipping silence-gap test"
fi

echo
echo "== test: DSP/MIR only see active spans, not the whole file (PRD §5) =="
if command -v ffmpeg >/dev/null 2>&1; then
    RESTRICT_DIR=$(mktemp -d)
    # Same 2s-silence/3s-tone/2s-silence/3s-tone/2s-silence fixture, analyzed twice: once
    # as a stem (active-region-restricted) and once as an ordinary <5min file (not
    # restricted) — the same bytes on disk, so any descriptor difference is entirely the
    # active-region restriction's doing, not a different input.
    ffmpeg -y -loglevel error \
        -f lavfi -i "anullsrc=r=44100:cl=mono:d=2" \
        -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=3" \
        -f lavfi -i "anullsrc=r=44100:cl=mono:d=2" \
        -f lavfi -i "sine=frequency=880:sample_rate=44100:duration=3" \
        -f lavfi -i "anullsrc=r=44100:cl=mono:d=2" \
        -filter_complex "[0][1][2][3][4]concat=n=5:v=0:a=1[out]" -map "[out]" \
        "$RESTRICT_DIR/tones.wav"

    RESTRICT_DB="$(mktemp -t mira_smoke_restrict_XXXXXX).db"
    "$MIRA" scan "$RESTRICT_DIR" --db "$RESTRICT_DB" --as stem >/dev/null 2>&1
    "$MIRA" analyze --db "$RESTRICT_DB" >/dev/null 2>&1
    CENTROID_RESTRICTED=$(sqlite3 "$RESTRICT_DB" "SELECT json_extract(machine,'\$.dsp.spectral_centroid_hz') FROM files")

    UNRESTRICT_DB="$(mktemp -t mira_smoke_unrestrict_XXXXXX).db"
    "$MIRA" scan "$RESTRICT_DIR" --db "$UNRESTRICT_DB" >/dev/null 2>&1
    "$MIRA" analyze --db "$UNRESTRICT_DB" >/dev/null 2>&1
    CENTROID_UNRESTRICTED=$(sqlite3 "$UNRESTRICT_DB" "SELECT json_extract(machine,'\$.dsp.spectral_centroid_hz') FROM files")

    DIFFERS=$(awk -v a="$CENTROID_RESTRICTED" -v b="$CENTROID_UNRESTRICTED" \
        'BEGIN{d=a-b; if(d<0) d=-d; print (d>1) ? "yes" : "no"}')
    assert_eq "same file's spectral centroid differs between stem (restricted) and ordinary (unrestricted) analysis" \
        "yes" "$DIFFERS"

    rm -rf "$RESTRICT_DIR"
    rm -f "$RESTRICT_DB" "$RESTRICT_DB-wal" "$RESTRICT_DB-shm" \
          "$UNRESTRICT_DB" "$UNRESTRICT_DB-wal" "$UNRESTRICT_DB-shm"
else
    echo "  SKIP: ffmpeg not found, skipping active-region-restriction test"
fi

echo
echo "== test: DSP descriptors — sanity-checked against a pure sine wave =="
if command -v ffmpeg >/dev/null 2>&1; then
    SINE_DIR=$(mktemp -d)
    # Mono, native 48kHz (not mira's old fixed-44.1kHz analysis rate) — also exercises
    # AudioLoader's mono->stereo channel duplication path.
    ffmpeg -y -loglevel error -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=2" \
        -ac 1 "$SINE_DIR/sine440.wav"
    rm -f "$TESTDB" "$TESTDB-wal" "$TESTDB-shm"
    "$MIRA" scan "$SINE_DIR" --db "$TESTDB" >/dev/null 2>&1
    "$MIRA" analyze --db "$TESTDB" >/dev/null 2>&1
    MACHINE=$(sqlite3 "$TESTDB" "SELECT machine FROM files")
    assert_eq "native sample rate preserved (not forced to 44.1kHz)" "48000" \
        "$(echo "$MACHINE" | python3 -c 'import json,sys; print(json.load(sys.stdin)["sample_rate"])')"
    assert_eq "mono channel count recorded correctly" "1" \
        "$(echo "$MACHINE" | python3 -c 'import json,sys; print(json.load(sys.stdin)["num_channels"])')"
    CENTROID=$(echo "$MACHINE" | python3 -c 'import json,sys; print(json.load(sys.stdin)["dsp"]["spectral_centroid_hz"])')
    CENTROID_OK=$(awk -v c="$CENTROID" 'BEGIN{print (c>400 && c<500) ? "yes" : "no"}')
    assert_eq "spectral centroid is near 440Hz for a 440Hz sine" "yes" "$CENTROID_OK"
    CREST=$(echo "$MACHINE" | python3 -c 'import json,sys; print(json.load(sys.stdin)["dsp"]["crest_factor"])')
    # A sine wave's peak/mean(|x|) is exactly pi/2 ~= 1.5708
    CREST_OK=$(awk -v c="$CREST" 'BEGIN{print (c>1.5 && c<1.65) ? "yes" : "no"}')
    assert_eq "crest factor matches the sine wave's theoretical pi/2" "yes" "$CREST_OK"
    HARMONICITY=$(echo "$MACHINE" | python3 -c 'import json,sys; print(json.load(sys.stdin)["dsp"]["harmonicity"])')
    assert_eq "a pure sine wave is maximally harmonic" "1" "$HARMONICITY"
    FRAME_COUNT=$(echo "$MACHINE" | python3 -c 'import json,sys; print(json.load(sys.stdin)["dsp"]["harmonicity_frame_count"])')
    assert_eq "harmonicity was actually measured (frame_count>0)" "yes" \
        "$(awk -v n="$FRAME_COUNT" 'BEGIN{print (n>0) ? "yes" : "no"}')"
    MFCC_LEN=$(echo "$MACHINE" | python3 -c 'import json,sys; print(len(json.load(sys.stdin)["dsp"]["mfcc"]))')
    assert_eq "MFCC has 13 coefficients" "13" "$MFCC_LEN"
    CHROMA_PEAK_BIN=$(echo "$MACHINE" | python3 -c 'import json,sys; c=json.load(sys.stdin)["dsp"]["chroma"]; print(c.index(max(c)))')
    # 440Hz is exactly A4; HPCP's default referenceFrequency=440 puts A's pitch class at bin 0.
    assert_eq "chroma peaks at the A bin for a 440Hz (A4) sine" "0" "$CHROMA_PEAK_BIN"
    rm -rf "$SINE_DIR"
else
    echo "  SKIP: ffmpeg not found, skipping DSP descriptor sanity test"
fi

echo
echo "== test: harmonicity is 0 with frame_count 0 on white noise (not falsely '0=inharmonic') =="
if command -v ffmpeg >/dev/null 2>&1; then
    NOISE_HARM_DIR=$(mktemp -d)
    ffmpeg -y -loglevel error -f lavfi -i "anoisesrc=color=white:sample_rate=44100:duration=3" \
        "$NOISE_HARM_DIR/noise.wav"
    NOISE_HARM_DB="$(mktemp -t mira_smoke_noiseharm_XXXXXX).db"
    "$MIRA" scan "$NOISE_HARM_DIR" --db "$NOISE_HARM_DB" >/dev/null 2>&1
    "$MIRA" analyze --db "$NOISE_HARM_DB" >/dev/null 2>&1
    MACHINE_NOISE_HARM=$(sqlite3 "$NOISE_HARM_DB" "SELECT machine FROM files")
    FRAME_COUNT_NOISE=$(echo "$MACHINE_NOISE_HARM" | python3 -c 'import json,sys; print(json.load(sys.stdin)["dsp"]["harmonicity_frame_count"])')
    assert_eq "no frame of white noise was confidently pitched" "0" "$FRAME_COUNT_NOISE"
    rm -rf "$NOISE_HARM_DIR"
    rm -f "$NOISE_HARM_DB" "$NOISE_HARM_DB-wal" "$NOISE_HARM_DB-shm"
else
    echo "  SKIP: ffmpeg not found, skipping harmonicity-on-noise test"
fi

echo
echo "== test: Phase 2 classification vertical slice (embedding + content gate + moodtheme) =="
CLASSIFY_DB="$(mktemp -t mira_smoke_classify_XXXXXX).db"
"$MIRA" scan "$ROOT/fixtures" --db "$CLASSIFY_DB" >/dev/null 2>&1
"$MIRA" analyze --db "$CLASSIFY_DB" >/dev/null 2>&1
MACHINE_CLASSIFY=$(sqlite3 "$CLASSIFY_DB" "SELECT machine FROM files WHERE path LIKE '%flamenco.wav'")
assert_contains "machine JSON has an embedding section" "$MACHINE_CLASSIFY" "\"embedding\""
EMBED_LEN=$(echo "$MACHINE_CLASSIFY" | python3 -c 'import json,sys; print(len(json.load(sys.stdin)["embedding"]["vector"]))')
assert_eq "embedding vector has 1280 dimensions" "1280" "$EMBED_LEN"
EMBED_NONZERO=$(echo "$MACHINE_CLASSIFY" | python3 -c 'import json,sys; v=json.load(sys.stdin)["embedding"]["vector"]; print("yes" if any(x!=0 for x in v) else "no")')
assert_eq "embedding vector is not all-zero" "yes" "$EMBED_NONZERO"
assert_contains "machine JSON has a content_gate section" "$MACHINE_CLASSIFY" "\"content_gate\""
IS_MUSIC=$(echo "$MACHINE_CLASSIFY" | python3 -c 'import json,sys; print(json.load(sys.stdin)["content_gate"]["is_music"])')
assert_eq "content gate says flamenco.wav is music" "True" "$IS_MUSIC"
assert_contains "machine JSON has a moodtheme section (gate passed)" "$MACHINE_CLASSIFY" "\"moodtheme\""
MOODTHEME_LEN=$(echo "$MACHINE_CLASSIFY" | python3 -c 'import json,sys; print(len(json.load(sys.stdin)["moodtheme"]))')
assert_eq "moodtheme has 56 class scores" "56" "$MOODTHEME_LEN"

echo
echo "== test: instrument label normalization (taxonomy/instrument-labels.yaml) =="
assert_contains "machine JSON has an instrument_normalized section" "$MACHINE_CLASSIFY" "\"instrument_normalized\""
NORM_CHECK=$(echo "$MACHINE_CLASSIFY" | python3 -c '
import json, sys
d = json.load(sys.stdin)
raw, norm = d["instrument"], d["instrument_normalized"]
checks = []
# Cross-model spelling fix: camelCase raw label -> spaced canonical term, same score.
checks.append(norm.get("electric guitar") == raw.get("electricguitar"))
checks.append(norm.get("acoustic guitar") == raw.get("acousticguitar"))
checks.append(norm.get("classical guitar") == raw.get("classicalguitar"))
# THE RULE under test: electric/acoustic/classical guitar must stay three distinct
# canonical terms, never collapsed into one "guitar" bucket for tidiness.
distinct_terms = {"electric guitar", "acoustic guitar", "classical guitar", "guitar"}
checks.append(distinct_terms.issubset(norm.keys()))
print("yes" if all(checks) else "no")
')
assert_eq "electric/acoustic/classical guitar stay distinct canonical terms, correctly renamed" "yes" "$NORM_CHECK"

echo
echo "== test: genre label normalization (taxonomy/genre-labels.yaml) =="
assert_contains "machine JSON has a genre_normalized section" "$MACHINE_CLASSIFY" "\"genre_normalized\""
GENRE_NORM_CHECK=$(echo "$MACHINE_CLASSIFY" | python3 -c '
import json, sys
d = json.load(sys.stdin)
raw, norm = d["genre"], d["genre_normalized"]
checks = []
# Mechanical "---" -> ": " fix, score unchanged, on the real top genre for flamenco.wav.
raw_key = "Folk, World, & Country---Flamenco"
norm_key = "Folk, World, & Country: Flamenco"
checks.append(raw_key in raw)
checks.append(norm.get(norm_key) == raw.get(raw_key))
# The raw "---" form must not leak into the normalized object.
checks.append(not any("---" in k for k in norm.keys()))
print("yes" if all(checks) else "no")
')
assert_eq "genre Genre---Style separator becomes Genre: Style, score preserved" "yes" "$GENRE_NORM_CHECK"

if command -v ffmpeg >/dev/null 2>&1; then
    NOISE_GATE_DIR=$(mktemp -d)
    # Fixed seed: an unseeded anoisesrc clip's CED-small music_score drifts run-to-run
    # (measured 0.37-0.44 across regenerations) close enough to kContentGateMusicThreshold
    # (0.45) to occasionally flip this assertion — seed=12345 measured at 0.385, a safe
    # margin below threshold.
    ffmpeg -y -loglevel error -f lavfi -i "anoisesrc=color=white:sample_rate=44100:duration=5:seed=12345" \
        "$NOISE_GATE_DIR/noise.wav"
    NOISE_GATE_DB="$(mktemp -t mira_smoke_noisegate_XXXXXX).db"
    "$MIRA" scan "$NOISE_GATE_DIR" --db "$NOISE_GATE_DB" >/dev/null 2>&1
    "$MIRA" analyze --db "$NOISE_GATE_DB" >/dev/null 2>&1
    MACHINE_NOISE_GATE=$(sqlite3 "$NOISE_GATE_DB" "SELECT machine FROM files")
    IS_MUSIC_NOISE=$(echo "$MACHINE_NOISE_GATE" | python3 -c 'import json,sys; print(json.load(sys.stdin)["content_gate"]["is_music"])')
    assert_eq "content gate says white noise is NOT music" "False" "$IS_MUSIC_NOISE"
    NOT_CONTAINS_MOODTHEME=$(echo "$MACHINE_NOISE_GATE" | grep -c "\"moodtheme\"" || true)
    assert_eq "moodtheme did not run on non-music (gated out)" "0" "$NOT_CONTAINS_MOODTHEME"
    rm -rf "$NOISE_GATE_DIR"
    rm -f "$NOISE_GATE_DB" "$NOISE_GATE_DB-wal" "$NOISE_GATE_DB-shm"
else
    echo "  SKIP: ffmpeg not found, skipping content-gate-on-noise test"
fi
rm -f "$CLASSIFY_DB" "$CLASSIFY_DB-wal" "$CLASSIFY_DB-shm"

echo
echo "== test: mira similar (sqlite-vec, exact brute-force KNN over the embedding) =="
if command -v ffmpeg >/dev/null 2>&1; then
    SIMILAR_DIR=$(mktemp -d)
    cp "$ROOT/fixtures/flamenco.wav" "$SIMILAR_DIR/flamenco.wav"
    # A 3s sine tone: long enough for a stored embedding (needs ~2s for one mel patch),
    # but acoustically nothing like flamenco.wav — a real distance should separate them.
    ffmpeg -y -loglevel error -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=3" \
        "$SIMILAR_DIR/tone.wav"
    # Too short for even one mel patch (~2.05s minimum) — exercises the "no stored
    # embedding" error path, not the "file not found" one.
    ffmpeg -y -loglevel error -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=0.5" \
        "$SIMILAR_DIR/tooshort.wav"
    SIMILAR_DB="$(mktemp -t mira_smoke_similar_XXXXXX).db"
    "$MIRA" scan "$SIMILAR_DIR" --db "$SIMILAR_DB" >/dev/null 2>&1
    "$MIRA" analyze --db "$SIMILAR_DB" >/dev/null 2>&1

    OUT_SIMILAR=$("$MIRA" similar "$SIMILAR_DIR/flamenco.wav" --db "$SIMILAR_DB" --n 5 2>&1)
    CODE_SIMILAR=$?
    assert_eq "exit code" "0" "$CODE_SIMILAR"
    assert_contains "returns the tone as a match" "$OUT_SIMILAR" "tone.wav"
    NOT_CONTAINS_SELF=$(echo "$OUT_SIMILAR" | grep -c "flamenco.wav$" || true)
    assert_eq "does not return the query file itself" "0" "$NOT_CONTAINS_SELF"

    OUT_NOTFOUND=$("$MIRA" similar "$SIMILAR_DIR/no-such-file.wav" --db "$SIMILAR_DB" 2>&1)
    CODE_NOTFOUND=$?
    assert_eq "exit code non-zero for a file not in the library" "1" "$CODE_NOTFOUND"
    assert_contains "explains why" "$OUT_NOTFOUND" "no file found"

    OUT_NOEMBED=$("$MIRA" similar "$SIMILAR_DIR/tooshort.wav" --db "$SIMILAR_DB" 2>&1)
    CODE_NOEMBED=$?
    assert_eq "exit code non-zero for a file with no stored embedding" "1" "$CODE_NOEMBED"
    assert_contains "explains why (too short for a mel patch)" "$OUT_NOEMBED" "no stored embedding"

    rm -rf "$SIMILAR_DIR"
    rm -f "$SIMILAR_DB" "$SIMILAR_DB-wal" "$SIMILAR_DB-shm"
else
    echo "  SKIP: ffmpeg not found, skipping mira similar test"
fi

echo
echo "== test: mira inspect =="
rm -f "$TESTDB" "$TESTDB-wal" "$TESTDB-shm"
"$MIRA" scan "$ROOT/fixtures" --db "$TESTDB" >/dev/null 2>&1
"$MIRA" analyze --db "$TESTDB" --chords --transcribe --recheck-tempo >/dev/null 2>&1
OUT=$("$MIRA" inspect "$ROOT/fixtures/flamenco.wav" --db "$TESTDB" 2>&1)
CODE=$?
assert_eq "exit code" "0" "$CODE"
assert_contains "shows content_type" "$OUT" "content_type:  loop"
assert_contains "shows duration" "$OUT" "duration:"
assert_contains "shows DSP section" "$OUT" "DSP:"
assert_contains "shows rhythm section" "$OUT" "Rhythm:"
assert_contains "flags the essentia/beat_this tempo disagreement" "$OUT" "estimators disagree"
assert_contains "shows key" "$OUT" "Key:"
assert_contains "shows chord segment count" "$OUT" "Chords:"
assert_contains "shows transcribed note count" "$OUT" "Notes:"

OUT_BY_ID=$("$MIRA" inspect 1 --db "$TESTDB" 2>&1)
assert_eq "inspecting by numeric id matches inspecting by path" "$OUT" "$OUT_BY_ID"

echo
echo "== test: mira inspect doesn't fabricate an essentia/disagreement line when --recheck-tempo wasn't used =="
# Regression test: essentia_bpm/bpm_ratio are always serialized (default 0.0 when
# unmeasured), so a naive "does this JSON field exist" presence check is always true even
# at 0 — this previously showed a fake "essentia: 0.00 BPM" and a spurious "estimators
# disagree (ratio 0.000)" on every file analyzed without --recheck-tempo. Found by
# inspecting a real file analyzed both ways back to back; the suite's other inspect test
# above always paired --recheck-tempo with inspect, which masked this.
NORECHECK_DB="$(mktemp -t mira_smoke_norecheck_XXXXXX).db"
"$MIRA" scan "$ROOT/fixtures" --db "$NORECHECK_DB" >/dev/null 2>&1
"$MIRA" analyze --db "$NORECHECK_DB" >/dev/null 2>&1
OUT_NORECHECK=$("$MIRA" inspect "$ROOT/fixtures/flamenco.wav" --db "$NORECHECK_DB" 2>&1)
assert_contains "still shows the default beat_this estimate" "$OUT_NORECHECK" "beat_this:"
assert_contains "explains only one estimator ran" "$OUT_NORECHECK" "single estimator"
NOT_CONTAINS=$(echo "$OUT_NORECHECK" | grep -c "essentia:" || true)
assert_eq "does not show a fabricated essentia BPM line" "0" "$NOT_CONTAINS"
NOT_CONTAINS_RATIO=$(echo "$OUT_NORECHECK" | grep -c "estimators disagree" || true)
assert_eq "does not show a spurious disagreement warning" "0" "$NOT_CONTAINS_RATIO"
rm -f "$NORECHECK_DB" "$NORECHECK_DB-wal" "$NORECHECK_DB-shm"

echo
echo "== test: tempo stability fields are well-formed (short fixture stays unmeasured) =="
# flamenco.wav is only 14.2s — far under the 3-window (~3 min) minimum, so tempo
# stability must stay unmeasured (window_count 0), not report a false "stable" (stddev 0).
MACHINE_FLAMENCO=$(sqlite3 "$TESTDB" "SELECT machine FROM files WHERE path LIKE '%flamenco.wav'")
TEMPO_WINDOWS=$(echo "$MACHINE_FLAMENCO" | python3 -c 'import json,sys; print(json.load(sys.stdin)["rhythm"]["tempo_window_count"])')
assert_eq "short file: tempo stability stays unmeasured (window_count 0)" "0" "$TEMPO_WINDOWS"
NOT_CONTAINS_UNSTABLE=$(echo "$OUT" | grep -c "tempo is not stable" || true)
assert_eq "short file: no tempo-instability warning shown" "0" "$NOT_CONTAINS_UNSTABLE"

echo
echo "== test: provenance (PRD §6) recorded per file =="
PROVENANCE=$(sqlite3 "$TESTDB" "SELECT provenance FROM files WHERE path LIKE '%flamenco.wav'")
assert_contains "provenance has mira_git_hash" "$PROVENANCE" "mira_git_hash"
assert_contains "provenance has the essentia version actually built" "$PROVENANCE" "2.1-beta6-dev"
GIT_HASH_IN_DB=$(echo "$PROVENANCE" | python3 -c 'import json,sys; print(json.load(sys.stdin)["mira_git_hash"])')
assert_eq "recorded git hash is not the placeholder" "no" \
    "$([ "$GIT_HASH_IN_DB" = "unknown" ] && echo yes || echo no)"

OUT_MISSING=$("$MIRA" inspect no-such-file.wav --db "$TESTDB" 2>&1)
CODE_MISSING=$?
if [[ "$CODE_MISSING" != "0" ]]; then pass "exit code non-zero for a file not in the library ($CODE_MISSING)"
else fail "expected non-zero exit code for a missing file, got 0"; fi

NOTYET_DB="$(mktemp -t mira_smoke_notyet_XXXXXX).db"
"$MIRA" scan "$ROOT/fixtures" --db "$NOTYET_DB" >/dev/null 2>&1
OUT_NOTYET=$("$MIRA" inspect "$ROOT/fixtures/flamenco.wav" --db "$NOTYET_DB" 2>&1)
assert_contains "scanned-but-not-analyzed file says so rather than crashing" "$OUT_NOTYET" "not yet analyzed"
rm -f "$NOTYET_DB" "$NOTYET_DB-wal" "$NOTYET_DB-shm"

echo
echo "== test: mira stats (PRD §8: library composition, coverage, unmapped labels) =="
OUT_STATS=$("$MIRA" stats --db "$TESTDB" 2>&1)
CODE_STATS=$?
assert_eq "exit code" "0" "$CODE_STATS"
assert_contains "reports library file count" "$OUT_STATS" "library:"
assert_contains "reports content type breakdown" "$OUT_STATS" "content type breakdown"
assert_contains "reports classification coverage" "$OUT_STATS" "classification coverage"
assert_contains "reports embeddings stored" "$OUT_STATS" "embeddings stored"
assert_contains "reports taxonomy completeness" "$OUT_STATS" "unmapped labels"
assert_contains "instrument taxonomy is fully mapped" "$OUT_STATS" "instrument (mtg_jamendo_instrument): none"
assert_contains "genre taxonomy is fully mapped" "$OUT_STATS" "genre (genre_discogs400): none"

echo
echo "== test: mira models --list (PRD §8) =="
OUT_MODELS=$("$MIRA" models --list 2>&1)
CODE_MODELS=$?
assert_eq "exit code (all models present in this dev environment)" "0" "$CODE_MODELS"
assert_contains "lists discogs-effnet" "$OUT_MODELS" "discogs-effnet"
assert_contains "lists CED-small" "$OUT_MODELS" "CED-small"
assert_contains "lists genre_discogs400" "$OUT_MODELS" "genre_discogs400"
NOT_MISSING=$(echo "$OUT_MODELS" | grep -c "MISSING" || true)
assert_eq "no models reported missing in this dev environment" "0" "$NOT_MISSING"

OUT_MODELS_NOFLAG=$("$MIRA" models 2>&1)
CODE_MODELS_NOFLAG=$?
assert_eq "exit code non-zero without --list or --download" "1" "$CODE_MODELS_NOFLAG"

echo
echo "== test: mira search --filter (PRD §8) =="
OUT_SEARCH_NUM=$("$MIRA" search --filter "content_type=loop" --db "$TESTDB" 2>&1)
assert_contains "numeric/string field filter matches flamenco.wav" "$OUT_SEARCH_NUM" "flamenco.wav"

OUT_SEARCH_TAG=$("$MIRA" search --filter "genre:Flamenco" --db "$TESTDB" 2>&1)
assert_contains "genre tag filter matches (substring of Genre: Style canonical form)" "$OUT_SEARCH_TAG" "flamenco.wav"

OUT_SEARCH_NOMATCH=$("$MIRA" search --filter "genre:Techno" --db "$TESTDB" 2>&1)
assert_contains "unrelated genre tag correctly finds no match" "$OUT_SEARCH_NOMATCH" "0 matches"
# Regression: a naive "substring appears anywhere in the 400-label object" check (an
# earlier, wrong implementation) matched this every time, since some near-zero-score
# genre label almost always contains any given substring somewhere in the full 400.
NOT_FALSE_MATCH=$(echo "$OUT_SEARCH_NOMATCH" | grep -c "flamenco.wav" || true)
assert_eq "unrelated genre tag does not false-match via a near-zero score" "0" "$NOT_FALSE_MATCH"

OUT_SEARCH_AND=$("$MIRA" search --filter "content_type=loop,bpm>0" --db "$TESTDB" 2>&1)
assert_contains "comma-separated AND conditions both apply" "$OUT_SEARCH_AND" "flamenco.wav"

OUT_SEARCH_BAD=$("$MIRA" search --filter "nonsense>5" --db "$TESTDB" 2>&1)
CODE_SEARCH_BAD=$?
assert_eq "exit code non-zero for an unknown field" "1" "$CODE_SEARCH_BAD"
assert_contains "explains why" "$OUT_SEARCH_BAD" "could not parse"

echo
echo "== test: mira caption (PRD §11/§15, SA3 renderer, TASKS.md Phase 3) =="
OUT_CAPTION=$("$MIRA" caption "$ROOT/fixtures/flamenco.wav" --db "$TESTDB" --trigger sks_test 2>&1)
CODE_CAPTION=$?
assert_eq "exit code" "0" "$CODE_CAPTION"
assert_contains "prose includes the trigger token" "$OUT_CAPTION" "sks_test,"
assert_contains "prose includes the TrackType prefix" "$OUT_CAPTION" "TrackType: Music"
assert_contains "prose includes the normalized genre" "$OUT_CAPTION" "Flamenco"
assert_contains "prose includes BPM" "$OUT_CAPTION" "BPM:"
assert_contains "prose includes Length in seconds" "$OUT_CAPTION" "Length:"
assert_contains "tags section lists genre" "$OUT_CAPTION" "genre:"
assert_contains "tags section lists keyscale" "$OUT_CAPTION" "keyscale: F minor"
# flamenco.wav's top moodtheme score is ~0.02 (see the mira-inspect test above), well
# under the caption render threshold -- a caption must not assert a mood it isn't
# confident about (PRD §12.6: confidence-gated field omission, not assertion).
NOT_MOOD=$(echo "$OUT_CAPTION" | grep -c "mood" || true)
assert_eq "low-confidence mood is omitted, not asserted" "0" "$NOT_MOOD"

OUT_CAPTION_BAD=$("$MIRA" caption "$ROOT/fixtures/nonexistent-file.wav" --db "$TESTDB" 2>&1)
CODE_CAPTION_BAD=$?
assert_eq "exit code non-zero for an unknown file" "1" "$CODE_CAPTION_BAD"

SIDECAR_PATH="$ROOT/fixtures/flamenco.json"
rm -f "$SIDECAR_PATH"
OUT_SIDECAR=$("$MIRA" caption "$ROOT/fixtures/flamenco.wav" --db "$TESTDB" --emit-sidecar 2>&1)
assert_contains "reports the sidecar path written" "$OUT_SIDECAR" "sidecar written"
if [[ -f "$SIDECAR_PATH" ]]; then
    pass "sidecar file was created next to the source audio"
    SIDECAR_CONTENT=$(cat "$SIDECAR_PATH")
    assert_contains "sidecar is a flat JSON object with a prompt key" "$SIDECAR_CONTENT" "\"prompt\":"
    # underfit's prompt_templates.py _get() silently keeps only the first element of a
    # list-valued tag -- every field mira writes must be a plain (comma-joined) string,
    # never a JSON array, or multi-value fields like genre/instruments would be truncated
    # to one entry the moment underfit reads this sidecar.
    NOT_ARRAY=$(echo "$SIDECAR_CONTENT" | grep -c '\[' || true)
    assert_eq "no field is serialized as a JSON array" "0" "$NOT_ARRAY"
else
    fail "sidecar file was created next to the source audio"
fi
rm -f "$SIDECAR_PATH"

echo
echo "== test: mira tag (PRD §6/§11, human overrides -- keywords has no machine source) =="
TAGDB="$(mktemp -t mira_smoke_tag_XXXXXX).db"
"$MIRA" scan "$ROOT/fixtures" --db "$TAGDB" >/dev/null 2>&1
"$MIRA" analyze --db "$TAGDB" --recheck-tempo >/dev/null 2>&1

OUT_TAG_EMPTY=$("$MIRA" tag 1 --db "$TAGDB" 2>&1)
CODE_TAG_EMPTY=$?
assert_eq "exit code non-zero with no flags and no --clear" "1" "$CODE_TAG_EMPTY"

OUT_TAG=$("$MIRA" tag 1 --db "$TAGDB" --keywords "funny, quirky" --moods "comedic" 2>&1)
CODE_TAG=$?
assert_eq "exit code" "0" "$CODE_TAG"
assert_contains "reports the keywords it set" "$OUT_TAG" "\"keywords\":[\"funny\",\"quirky\"]"
assert_contains "reports the moods it set" "$OUT_TAG" "\"moods\":[\"comedic\"]"

OUT_CAPTION_TAGGED=$("$MIRA" caption 1 --db "$TAGDB" 2>&1)
assert_contains "caption prose folds in the human keyword" "$OUT_CAPTION_TAGGED" "funny"
assert_contains "caption prose folds in the human mood override" "$OUT_CAPTION_TAGGED" "comedic"
assert_contains "tags section lists keywords separately from moods" "$OUT_CAPTION_TAGGED" "keywords: funny, quirky"

# A second `mira tag` call for a different field must not clobber the first.
"$MIRA" tag 1 --db "$TAGDB" --key "F minor" >/dev/null 2>&1
OUT_TAG_MERGED=$("$MIRA" inspect 1 --db "$TAGDB" 2>&1)
assert_contains "merged human override still has the earlier keywords call" "$OUT_TAG_MERGED" "funny"
assert_contains "merged human override also has the later key call" "$OUT_TAG_MERGED" "F minor"

"$MIRA" tag 1 --db "$TAGDB" --clear >/dev/null 2>&1
OUT_CAPTION_CLEARED=$("$MIRA" caption 1 --db "$TAGDB" 2>&1)
NOT_FUNNY=$(echo "$OUT_CAPTION_CLEARED" | grep -c "funny" || true)
assert_eq "--clear removes human overrides from the rendered caption" "0" "$NOT_FUNNY"

rm -f "$TAGDB" "$TAGDB-wal" "$TAGDB-shm"

echo
echo "== test: mira tag-segment + export-segments (TASKS.md Phase 3 addition -- SA3 has no"
echo "   per-clip timeline conditioning, so a file whose character changes partway through"
echo "   can only be captioned by cutting it into per-segment clips) =="
SEGDB="$(mktemp -t mira_smoke_seg_XXXXXX).db"
"$MIRA" scan "$ROOT/fixtures" --db "$SEGDB" >/dev/null 2>&1
"$MIRA" analyze --db "$SEGDB" --recheck-tempo >/dev/null 2>&1

OUT_SEGTAG_BAD_RANGE=$("$MIRA" tag-segment 1 --db "$SEGDB" --start 7 --end 3 2>&1)
CODE_SEGTAG_BAD_RANGE=$?
assert_eq "exit code non-zero when --end <= --start" "1" "$CODE_SEGTAG_BAD_RANGE"

OUT_SEGTAG_1=$("$MIRA" tag-segment 1 --db "$SEGDB" --start 0 --end 7 --keywords "intro, gentle" 2>&1)
CODE_SEGTAG_1=$?
assert_eq "exit code" "0" "$CODE_SEGTAG_1"
assert_contains "reports the segment id created" "$OUT_SEGTAG_1" "segment 1 created"
assert_contains "reports the declared time range" "$OUT_SEGTAG_1" "0s-7s"

"$MIRA" tag-segment 1 --db "$SEGDB" --start 7 --end 14 --keywords "energetic, driving" >/dev/null 2>&1

SEGOUT="$(mktemp -d -t mira_smoke_segout_XXXXXX)"
OUT_EXPORT=$("$MIRA" export-segments 1 --db "$SEGDB" --out-dir "$SEGOUT" --trigger sks_test 2>&1)
CODE_EXPORT=$?
assert_eq "exit code" "0" "$CODE_EXPORT"
assert_contains "reports 2 clips written" "$OUT_EXPORT" "2 clips written"

SEG1_WAV="$SEGOUT/seg1_0-7s/flamenco.wav"
SEG2_WAV="$SEGOUT/seg2_7-14s/flamenco.wav"
if [[ -f "$SEG1_WAV" && -f "$SEG2_WAV" ]]; then
    pass "both segment WAV files were written"
else
    fail "both segment WAV files were written"
fi

SEG1_JSON="$SEGOUT/seg1_0-7s/flamenco.json"
if [[ -f "$SEG1_JSON" ]]; then
    pass "segment sidecar JSON was written"
    SEG1_CONTENT=$(cat "$SEG1_JSON")
    assert_contains "segment sidecar has the segment's own keywords" "$SEG1_CONTENT" "intro, gentle"
    assert_contains "segment sidecar's length reflects the cut, not the whole file" "$SEG1_CONTENT" "\"length_seconds\":\"7\""
    assert_contains "segment sidecar carries the trigger token" "$SEG1_CONTENT" "sks_test"
else
    fail "segment sidecar JSON was written"
fi

# Each cut clip must actually be ~7s of real audio, not the whole 14s file duplicated --
# use ffprobe (already a build dependency) rather than trusting the reported byte count.
if command -v ffprobe >/dev/null 2>&1; then
    SEG1_DUR=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$SEG1_WAV" 2>/dev/null)
    SEG1_DUR_ROUNDED=$(printf "%.0f" "${SEG1_DUR:-0}")
    assert_eq "segment 1's cut audio is actually ~7 seconds long" "7" "$SEG1_DUR_ROUNDED"
fi

OUT_EXPORT_NONE=$("$MIRA" export-segments 999999 --db "$SEGDB" --out-dir "$SEGOUT" 2>&1)
CODE_EXPORT_NONE=$?
assert_eq "exit code non-zero for a nonexistent target" "1" "$CODE_EXPORT_NONE"

rm -rf "$SEGOUT"
rm -f "$SEGDB" "$SEGDB-wal" "$SEGDB-shm"

echo
echo "== test: mira tag-folder (TASKS.md Phase 3 addition -- tagging a whole library"
echo "   without one mira-tag call per file) =="
FOLDDB="$(mktemp -t mira_smoke_folder_XXXXXX).db"
"$MIRA" scan "$ROOT/fixtures" --db "$FOLDDB" >/dev/null 2>&1
"$MIRA" analyze --db "$FOLDDB" >/dev/null 2>&1

OUT_TAGFOLDER_NOTDIR=$("$MIRA" tag-folder "$ROOT/fixtures/flamenco.wav" --db "$FOLDDB" --keywords "x" 2>&1)
CODE_TAGFOLDER_NOTDIR=$?
assert_eq "exit code non-zero when the target isn't a directory" "1" "$CODE_TAGFOLDER_NOTDIR"

OUT_TAGFOLDER=$("$MIRA" tag-folder "$ROOT/fixtures" --db "$FOLDDB" --keywords "score-cue, demo" --genre "Folder Genre" 2>&1)
CODE_TAGFOLDER=$?
assert_eq "exit code" "0" "$CODE_TAGFOLDER"

OUT_CAPTION_FOLDER=$("$MIRA" caption "$ROOT/fixtures/flamenco.wav" --db "$FOLDDB" 2>&1)
assert_contains "caption picks up the folder-default genre" "$OUT_CAPTION_FOLDER" "Folder Genre"
assert_contains "caption picks up the folder-default keywords" "$OUT_CAPTION_FOLDER" "score-cue"

# A file's own tag must win over the folder default for the same field, without losing
# the folder default's other fields (keywords).
"$MIRA" tag "$ROOT/fixtures/flamenco.wav" --db "$FOLDDB" --genre "Per-File Genre" >/dev/null 2>&1
OUT_CAPTION_OVERRIDE=$("$MIRA" caption "$ROOT/fixtures/flamenco.wav" --db "$FOLDDB" 2>&1)
assert_contains "per-file genre wins over the folder default" "$OUT_CAPTION_OVERRIDE" "Per-File Genre"
NOT_FOLDER_GENRE=$(echo "$OUT_CAPTION_OVERRIDE" | grep -c "Folder Genre" || true)
assert_eq "folder-default genre no longer shows once overridden" "0" "$NOT_FOLDER_GENRE"
assert_contains "folder-default keywords still applies (field wasn't overridden)" "$OUT_CAPTION_OVERRIDE" "score-cue"

# CAPTION-TAGGING.md: the four controlled vocabularies. Fixed lists exist so a LoRA is
# never taught two spellings of the same idea, which means a typo has to fail the whole
# command rather than half-tagging a folder.
OUT_VOCAB_BAD=$("$MIRA" tag-folder "$ROOT/fixtures" --db "$FOLDDB" --world "fantsy" 2>&1)
CODE_VOCAB_BAD=$?
assert_eq "exit code non-zero for a value outside the vocabulary" "1" "$CODE_VOCAB_BAD"
assert_contains "names the offending value" "$OUT_VOCAB_BAD" "fantsy"
assert_contains "lists the valid values" "$OUT_VOCAB_BAD" "post-apocalyptic"

OUT_VOCAB_3WORLD=$("$MIRA" tag-folder "$ROOT/fixtures" --db "$FOLDDB" --world "fantasy,noir,horror" 2>&1)
CODE_VOCAB_3WORLD=$?
assert_eq "exit code non-zero for more than two worlds" "1" "$CODE_VOCAB_3WORLD"

# A rejected command must not have written anything -- validation runs before the first
# setFolderDefaultField call.
OUT_CAPTION_NOWRITE=$("$MIRA" caption "$ROOT/fixtures/flamenco.wav" --db "$FOLDDB" 2>&1)
NOT_PARTIAL=$(echo "$OUT_CAPTION_NOWRITE" | grep -c "fantasy" || true)
assert_eq "a rejected tag-folder wrote nothing" "0" "$NOT_PARTIAL"

"$MIRA" tag-folder "$ROOT/fixtures" --db "$FOLDDB" --material score --world "fantasy,noir" \
    --harmonic heroic --signature wide-rubato >/dev/null 2>&1
OUT_CAPTION_VOCAB=$("$MIRA" caption "$ROOT/fixtures/flamenco.wav" --db "$FOLDDB" 2>&1)
assert_contains "material lands in keywords" "$OUT_CAPTION_VOCAB" "score"
assert_contains "both worlds land in keywords" "$OUT_CAPTION_VOCAB" "noir"
assert_contains "harmonic lands in keywords" "$OUT_CAPTION_VOCAB" "heroic"
assert_contains "signature lands in keywords" "$OUT_CAPTION_VOCAB" "wide-rubato"

# The two measured shape fields (CAPTION-TAGGING.md). They are legitimately ABSENT on a
# file with too little instrument mass or fewer than 16 beats -- PRD 12.6's "never assert
# what wasn't measured" -- so this checks the value is always from the bucket set when
# present, rather than demanding presence on a short fixture.
PALETTE_LINE=$(echo "$OUT_CAPTION_VOCAB" | grep "palette:" || true)
if [ -n "$PALETTE_LINE" ]; then
    PALETTE_OK=$(echo "$PALETTE_LINE" | grep -cE "palette: (acoustic|hybrid|electronic)$" || true)
    assert_eq "palette is one of acoustic/hybrid/electronic" "1" "$PALETTE_OK"
else
    pass "palette omitted rather than guessed (no usable instrument mass)"
fi
TIMING_LINE=$(echo "$OUT_CAPTION_VOCAB" | grep "timing:" || true)
if [ -n "$TIMING_LINE" ]; then
    TIMING_OK=$(echo "$TIMING_LINE" | grep -cE "timing: (tight|human|loose)$" || true)
    assert_eq "timing is one of tight/human/loose" "1" "$TIMING_OK"
else
    pass "timing omitted rather than guessed (no usable beat track)"
fi

"$MIRA" tag-folder "$ROOT/fixtures" --db "$FOLDDB" --clear >/dev/null 2>&1
"$MIRA" tag "$ROOT/fixtures/flamenco.wav" --db "$FOLDDB" --clear >/dev/null 2>&1
OUT_CAPTION_CLEARED_FOLDER=$("$MIRA" caption "$ROOT/fixtures/flamenco.wav" --db "$FOLDDB" 2>&1)
NOT_SCORE_CUE=$(echo "$OUT_CAPTION_CLEARED_FOLDER" | grep -c "score-cue" || true)
assert_eq "clearing the folder default removes it from the caption" "0" "$NOT_SCORE_CUE"

rm -f "$FOLDDB" "$FOLDDB-wal" "$FOLDDB-shm"

echo
echo "== test: export-segments surfaces a low-active-fraction note (active-region-aware"
echo "   boundary validation, TASKS.md Phase 3 addition) =="
if command -v ffmpeg >/dev/null 2>&1; then
    ACTIVEWARN_DIR=$(mktemp -d)
    # Same 2s-silence/3s-tone/2s-silence/3s-tone/2s-silence fixture used by the
    # active-region tests above: a segment declared entirely inside a silence gap should
    # get the low-active-fraction note; a segment inside a tone span shouldn't.
    ffmpeg -y -loglevel error \
        -f lavfi -i "anullsrc=r=44100:cl=mono:d=2" \
        -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=3" \
        -f lavfi -i "anullsrc=r=44100:cl=mono:d=2" \
        -f lavfi -i "sine=frequency=880:sample_rate=44100:duration=3" \
        -f lavfi -i "anullsrc=r=44100:cl=mono:d=2" \
        -filter_complex "[0][1][2][3][4]concat=n=5:v=0:a=1[out]" -map "[out]" \
        "$ACTIVEWARN_DIR/tones.wav"

    ACTIVEWARN_DB="$(mktemp -t mira_smoke_activewarn_XXXXXX).db"
    "$MIRA" scan "$ACTIVEWARN_DIR" --db "$ACTIVEWARN_DB" --as stem >/dev/null 2>&1
    "$MIRA" analyze --db "$ACTIVEWARN_DB" >/dev/null 2>&1
    "$MIRA" tag-segment 1 --db "$ACTIVEWARN_DB" --start 0 --end 2 >/dev/null 2>&1   # pure silence gap
    "$MIRA" tag-segment 1 --db "$ACTIVEWARN_DB" --start 2 --end 5 >/dev/null 2>&1   # pure tone span

    ACTIVEWARN_OUT="$(mktemp -d -t mira_smoke_activewarn_out_XXXXXX)"
    OUT_ACTIVEWARN=$("$MIRA" export-segments 1 --db "$ACTIVEWARN_DB" --out-dir "$ACTIVEWARN_OUT" 2>&1)
    assert_contains "silent segment gets the low-active-fraction note" "$OUT_ACTIVEWARN" "mostly silent here"

    # The tone-span segment's line is the one right after the silence segment's note —
    # check it specifically rather than the whole output, so a false positive on the
    # first segment can't accidentally satisfy this assertion too.
    TONE_LINE=$(echo "$OUT_ACTIVEWARN" | grep "seg2_2-5s")
    NOT_WARNED=$(echo "$TONE_LINE" | grep -c "mostly silent" || true)
    assert_eq "tone segment does not get the low-active-fraction note" "0" "$NOT_WARNED"

    rm -rf "$ACTIVEWARN_DIR" "$ACTIVEWARN_OUT"
    rm -f "$ACTIVEWARN_DB" "$ACTIVEWARN_DB-wal" "$ACTIVEWARN_DB-shm"
else
    echo "  SKIP: ffmpeg not found, skipping active-fraction warning test"
fi

echo
echo
echo "== test: RoBERTa tokenizer parity (analyze/RobertaTokenizer.cpp vs HuggingFace) =="
echo "   The CLAP text tower takes token ids, not text -- ids that are subtly wrong still"
echo "   produce a confident vector, just a meaningless one, so this is checked exactly."
DCLAP_DIR="$ROOT/models/similarity-embeddings/dclap"
if [[ -f "$DCLAP_DIR/roberta-vocab.tsv" && -f "$DCLAP_DIR/roberta-parity.tsv" ]]; then
    TOKBIN="$(mktemp -t mira_tokparity_XXXXXX)"
    if c++ -std=c++17 -O1 -o "$TOKBIN" "$ROOT/tests/tokenizer_parity.cpp" \
            "$ROOT/src/mira/analyze/RobertaTokenizer.cpp" 2>/dev/null; then
        OUT_TOK=$("$TOKBIN" "$DCLAP_DIR/roberta-vocab.tsv" "$DCLAP_DIR/roberta-merges.txt" \
                             "$DCLAP_DIR/roberta-parity.tsv" 2>&1)
        CODE_TOK=$?
        echo "$OUT_TOK"
        assert_eq "every tokenizer parity case matches HuggingFace" "0" "$CODE_TOK"
    else
        fail "tokenizer_parity.cpp did not compile"
    fi
    rm -f "$TOKBIN"
else
    echo "  SKIP: tokenizer tables not present (run lab/export_roberta_tokenizer.py)"
fi
echo "======================================"
echo "  $PASS passed, $FAIL failed"
echo "======================================"
[[ "$FAIL" -eq 0 ]]
