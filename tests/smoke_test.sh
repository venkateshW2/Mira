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
echo "== test: analyze routes a file by duration (flamenco.wav -> loop, 14.2s) =="
"$MIRA" scan "$ROOT/fixtures" --db "$TESTDB" >/dev/null 2>&1
OUT=$("$MIRA" analyze --db "$TESTDB" 2>&1)
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
    "$MIRA" analyze --db "$NOISE_DB" >/dev/null 2>&1
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
    "$MIRA" analyze --db "$TRANSCRIBE_NOISE_DB" >/dev/null 2>&1
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
    if run_with_timeout 15 "$MIRA" analyze --db "$SILENT_DB" >/dev/null 2>&1; then
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
    rm -rf "$SINE_DIR"
else
    echo "  SKIP: ffmpeg not found, skipping DSP descriptor sanity test"
fi

echo
echo "== test: mira inspect =="
rm -f "$TESTDB" "$TESTDB-wal" "$TESTDB-shm"
"$MIRA" scan "$ROOT/fixtures" --db "$TESTDB" >/dev/null 2>&1
"$MIRA" analyze --db "$TESTDB" >/dev/null 2>&1
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
echo "======================================"
echo "  $PASS passed, $FAIL failed"
echo "======================================"
[[ "$FAIL" -eq 0 ]]
