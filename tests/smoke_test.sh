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
assert_contains "reports nothing to route" "$OUT" "nothing to route"

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
echo "== test: --as stem declaration always overrides the router =="
rm -f "$TESTDB" "$TESTDB-wal" "$TESTDB-shm"
"$MIRA" scan "$ROOT/fixtures" --db "$TESTDB" --as stem >/dev/null 2>&1
CONTENT_TYPE=$(sqlite3 "$TESTDB" "SELECT content_type FROM files WHERE path LIKE '%flamenco.wav'")
CT_SOURCE=$(sqlite3 "$TESTDB" "SELECT content_type_source FROM files WHERE path LIKE '%flamenco.wav'")
assert_eq "content_type is stem immediately after scan" "stem" "$CONTENT_TYPE"
assert_eq "content_type_source is declared" "declared" "$CT_SOURCE"
OUT=$("$MIRA" analyze --db "$TESTDB" 2>&1)
assert_contains "analyze finds nothing to route (declared stems are never routed)" "$OUT" "nothing to route"
CONTENT_TYPE_AFTER=$(sqlite3 "$TESTDB" "SELECT content_type FROM files WHERE path LIKE '%flamenco.wav'")
assert_eq "content_type still stem after analyze" "stem" "$CONTENT_TYPE_AFTER"

echo
echo "======================================"
echo "  $PASS passed, $FAIL failed"
echo "======================================"
[[ "$FAIL" -eq 0 ]]
