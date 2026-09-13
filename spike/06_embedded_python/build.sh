#!/usr/bin/env bash
# build.sh — assemble, sign and verify the embedded-Python spike.
#   ./build.sh            ad-hoc sign + hardened runtime  (the default test)
#   ./build.sh --lax      additionally disable library validation (fallback)
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

APP="build/MiraPySpike.app"
PYROOT="pyruntime/cpython-3.11.14-macos-aarch64-none"
LAX=0; [ "${1:-}" = "--lax" ] && LAX=1

echo "── 1. assemble bundle ───────────────────────────────────────────"
rm -rf build; mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
clang++ -std=c++17 -O2 -o "$APP/Contents/MacOS/spike_host" host.cpp
cp worker.py "$APP/Contents/Resources/"
cp -R "$PYROOT" "$APP/Contents/Resources/python"
cp -R pysite    "$APP/Contents/Resources/pysite"
# Trim what never ships: pip's own tooling and bytecode caches.
rm -rf "$APP/Contents/Resources/python/lib/python3.11/test" \
       "$APP/Contents/Resources/python/lib/python3.11/idlelib"
find "$APP" -name '__pycache__' -type d -prune -exec rm -rf {} + 2>/dev/null || true

cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleExecutable</key><string>spike_host</string>
  <key>CFBundleIdentifier</key><string>app.mira.spike.embeddedpython</string>
  <key>CFBundleName</key><string>MiraPySpike</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
</dict></plist>
PLIST
echo "   bundle size: $(du -sh "$APP" | cut -f1)"

echo "── 2. sign inside-out ───────────────────────────────────────────"
# Every Mach-O must be signed before the bundle that contains it. Ad-hoc ('-')
# stands in for a Developer ID cert: it exercises the same hardened-runtime and
# library-validation rules, which is the part that can actually fail.
ENTS="entitlements.plist"
[ "$LAX" = 1 ] && ENTS="entitlements-lax.plist"
COUNT=0
while IFS= read -r f; do
  codesign --force --timestamp=none --options runtime \
           --entitlements "$ENTS" -s - "$f" 2>/dev/null && COUNT=$((COUNT+1))
done < <(find "$APP" \( -name '*.dylib' -o -name '*.so' \) -type f)
echo "   signed $COUNT nested Mach-O files"
codesign --force --timestamp=none --options runtime \
         --entitlements "$ENTS" -s - "$APP/Contents/Resources/python/bin/python3.11"
codesign --force --timestamp=none --options runtime \
         --entitlements "$ENTS" -s - "$APP"

echo "── 3. verify ────────────────────────────────────────────────────"
codesign --verify --deep --strict --verbose=2 "$APP" 2>&1 | sed 's/^/   /'
echo "   runtime flag: $(codesign -d --verbose=2 "$APP" 2>&1 | grep -o 'flags=.*' || echo '?')"

echo "── 4. RUN (the actual test) ─────────────────────────────────────"
"$APP/Contents/MacOS/spike_host"

echo "── 5. signature survives a run? ─────────────────────────────────"
# Regression guard for the bytecode trap: if the interpreter writes .pyc files
# into Contents/Resources, the seal breaks and Gatekeeper rejects the app.
PYC=$(find "$APP" -name '*.pyc' | wc -l | tr -d ' ')
echo "   .pyc written into bundle: $PYC  (must be 0)"
codesign --verify --deep --strict "$APP" 2>&1 | sed 's/^/   /'
codesign --verify --deep --strict "$APP" 2>/dev/null \
  && echo "   PASS — seal intact after running" \
  || { echo "   FAIL — seal broken by running"; exit 1; }
