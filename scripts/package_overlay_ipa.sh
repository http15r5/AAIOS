#!/bin/bash
# Package the compiled Husk executable and base release bundle into Husk.ipa and Husk.tipa
set -euo pipefail

HUSK_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT_DIR="$HUSK_ROOT/output"
mkdir -p "$OUT_DIR"

STAGE="/tmp/husk_stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/Payload"

# Copy base official bundle with all QEMU, ANGLE, widgets, and guest files
cp -R /tmp/husk_base/Payload/Husk.app "$STAGE/Payload/"

# Overlay our newly compiled app binary containing our custom Translation Layer
NEW_BIN="/tmp/husk_derived/Build/Products/Release-iphoneos/Husk.app/Husk"
if [ ! -f "$NEW_BIN" ]; then
    NEW_BIN="/tmp/husk_derived/Build/Products/Release-iphoneos/Husk"
fi
if [ -f "$NEW_BIN" ]; then
    echo "==> Replacing Husk executable with our newly compiled binary ($NEW_BIN)"
    cp -f "$NEW_BIN" "$STAGE/Payload/Husk.app/Husk"
else
    echo "Error: newly compiled binary not found"
    find /tmp/husk_derived/Build/Products -type f -name "Husk" || true
    exit 1
fi

# If custom Translation Layer built updated dylibs/frameworks, overlay them
if [ -d "/tmp/husk_derived/Build/Products/Release-iphoneos/Husk.app/Frameworks" ]; then
    cp -R /tmp/husk_derived/Build/Products/Release-iphoneos/Husk.app/Frameworks/* "$STAGE/Payload/Husk.app/Frameworks/" 2>/dev/null || true
fi

# Remove PlugIns (app extensions like widgets/helpers) that free Apple accounts cannot sign,
# which causes SideStore error 8220 (no devices to generate profile for extensions).
rm -rf "$STAGE/Payload/Husk.app/PlugIns"

# Sign ad-hoc with entitlements
ENT="$STAGE/entitlements.plist"
cp "$HUSK_ROOT/src/app/Husk/Husk.entitlements" "$ENT"
sign() { codesign --force --sign - --timestamp=none "$@"; }
find "$STAGE/Payload/Husk.app" -name "*.dylib" -not -path "*/Frameworks/*.framework/*" | while read -r f; do sign "$f"; done
for fw in "$STAGE/Payload/Husk.app"/Frameworks/*.framework; do [ -d "$fw" ] && sign "$fw"; done
sign --entitlements "$ENT" "$STAGE/Payload/Husk.app"

# Create standard IPA
( cd "$STAGE" && zip -qry "$OUT_DIR/Husk.ipa" Payload )
echo "==> Created $OUT_DIR/Husk.ipa"

# Create TrollStore TIPA (strip banned entitlements)
for key in "com.apple.private.cs.debugger" "dynamic-codesigning" "com.apple.private.skip-library-validation"; do
    plutil -remove "$key" "$ENT" 2>/dev/null || true
done
sign --entitlements "$ENT" "$STAGE/Payload/Husk.app"
( cd "$STAGE" && zip -qry "$OUT_DIR/Husk.tipa" Payload )
echo "==> Created $OUT_DIR/Husk.tipa"
