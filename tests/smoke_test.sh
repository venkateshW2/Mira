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
echo "======================================"
echo "  $PASS passed, $FAIL failed"
echo "======================================"
[[ "$FAIL" -eq 0 ]]
