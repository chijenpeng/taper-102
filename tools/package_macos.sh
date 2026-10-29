#!/bin/sh
# Builds and packages the macOS universal AU + VST3 from a clean, committed source tree.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/build-release"
DIST="$BUILD/dist"
PRODUCT="Taper 102"
VERSION=$(sed -n 's/^project(Taper102 VERSION \([0-9][0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")
BUNDLE_ID=$(sed -n 's/^[[:space:]]*BUNDLE_ID "\([^"]*\)".*/\1/p' "$ROOT/CMakeLists.txt")
MANUFACTURER=$(sed -n 's/^[[:space:]]*PLUGIN_MANUFACTURER_CODE \([^[:space:]]*\).*/\1/p' "$ROOT/CMakeLists.txt")
PLUGIN_CODE=$(sed -n 's/^[[:space:]]*PLUGIN_CODE \([^[:space:]]*\).*/\1/p' "$ROOT/CMakeLists.txt")
test -n "$VERSION" && test -n "$BUNDLE_ID" && test -n "$MANUFACTURER" && test -n "$PLUGIN_CODE" || {
    echo "Could not determine CMake project version/identity" >&2; exit 1;
}

git -C "$ROOT" rev-parse --verify HEAD >/dev/null 2>&1 || { echo "No source commit exists" >&2; exit 1; }
test -z "$(git -C "$ROOT" status --porcelain)" || { echo "Refusing to package a dirty working tree" >&2; exit 1; }
COMMIT=$(git -C "$ROOT" rev-parse HEAD)

JUCE="$ROOT/third_party/JUCE"
test -f "$JUCE/LICENSE.md" || JUCE=$(find "$BUILD/_deps" -maxdepth 1 -name 'juce-src' 2>/dev/null | head -n 1)
test -f "$JUCE/LICENSE.md" || { echo "JUCE checkout not found" >&2; exit 1; }
SDK="$JUCE/modules/juce_audio_processors/format_types/VST3_SDK"

if [ ! -f "$BUILD/CMakeCache.txt" ]; then
    cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCOPY_PLUGIN_AFTER_BUILD=OFF
fi
cmake --build "$BUILD" --target Taper102_AU Taper102_VST3 -j3

NAME="taper-102-$VERSION-macos-universal"
STAGE="$DIST/$NAME"
ARCHIVE="$DIST/$NAME.zip"
AU="$BUILD/Taper102_artefacts/Release/AU/$PRODUCT.component"
VST3="$BUILD/Taper102_artefacts/Release/VST3/$PRODUCT.vst3"

for path in "$AU" "$VST3"; do
    test -d "$path" || { echo "Missing built plug-in: $path" >&2; exit 1; }
    plist="$path/Contents/Info.plist"
    for key in CFBundleVersion CFBundleShortVersionString; do
        actual=$(plutil -extract "$key" raw -o - "$plist")
        test "$actual" = "$VERSION" || { echo "Stale $key in $path: $actual (expected $VERSION)" >&2; exit 1; }
    done
    actual=$(plutil -extract CFBundleIdentifier raw -o - "$plist")
    test "$actual" = "$BUNDLE_ID" || { echo "Stale bundle identity in $path: $actual (expected $BUNDLE_ID)" >&2; exit 1; }
done
actual=$(plutil -extract AudioComponents.0.manufacturer raw -o - "$AU/Contents/Info.plist")
test "$actual" = "$MANUFACTURER" || { echo "Stale AU manufacturer: $actual" >&2; exit 1; }
actual=$(plutil -extract AudioComponents.0.subtype raw -o - "$AU/Contents/Info.plist")
test "$actual" = "$PLUGIN_CODE" || { echo "Stale AU subtype: $actual" >&2; exit 1; }
for path in "$AU/Contents/MacOS/$PRODUCT" "$VST3/Contents/MacOS/$PRODUCT"; do
    arches=$(lipo -archs "$path")
    printf '%s\n' "$arches" | grep -qw arm64 || { echo "Missing arm64 in $path; got $arches" >&2; exit 1; }
    printf '%s\n' "$arches" | grep -qw x86_64 || { echo "Missing x86_64 in $path; got $arches" >&2; exit 1; }
done
for path in "$AU" "$VST3"; do
    codesign --verify --deep --strict "$path" 2>/dev/null || codesign --force --deep --sign - "$path"
    codesign --verify --deep --strict "$path"
done

rm -rf "$STAGE"
mkdir -p "$STAGE/licenses/VST3-SDK"
ditto "$AU" "$STAGE/$PRODUCT.component"
ditto "$VST3" "$STAGE/$PRODUCT.vst3"
cp "$ROOT/README.md" "$ROOT/THIRD_PARTY_NOTICES.md" "$STAGE/"
cp "$ROOT/Resources/fonts/Inter-LICENSE.txt" "$ROOT/Resources/fonts/JetBrainsMono-LICENSE.txt" "$STAGE/licenses/"
cp "$JUCE/LICENSE.md" "$STAGE/licenses/JUCE-LICENSE.md"
cp "$SDK/LICENSE.txt" "$STAGE/licenses/VST3-SDK/VST3-SDK-LICENSE.txt"
cp "$SDK/base/LICENSE.txt" "$STAGE/licenses/VST3-SDK/base-LICENSE.txt"
cp "$SDK/pluginterfaces/LICENSE.txt" "$STAGE/licenses/VST3-SDK/pluginterfaces-LICENSE.txt"
cp "$SDK/public.sdk/LICENSE.txt" "$STAGE/licenses/VST3-SDK/public-sdk-LICENSE.txt"
cp "$SDK/VST3_License_Agreement.pdf" "$STAGE/licenses/VST3-SDK/"
codesign --force --deep --sign - "$STAGE/$PRODUCT.component"
codesign --force --deep --sign - "$STAGE/$PRODUCT.vst3"
codesign --verify --deep --strict "$STAGE/$PRODUCT.component"
codesign --verify --deep --strict "$STAGE/$PRODUCT.vst3"

cat > "$STAGE/INSTALL.txt" <<TXT
$PRODUCT $VERSION - macOS universal

This package contains AU and VST3 bundles for arm64 and x86_64 Macs.
Copy $PRODUCT.component to ~/Library/Audio/Plug-Ins/Components/ and
$PRODUCT.vst3 to ~/Library/Audio/Plug-Ins/VST3/, then rescan plug-ins in
your DAW. The bundles are ad-hoc signed, not Developer ID signed or notarized,
so macOS may ask you to approve them in System Settings > Privacy & Security.

See README.md for controls and limits, THIRD_PARTY_NOTICES.md and licenses/
for dependency notices, and BUILD-INFO.txt for build provenance.
TXT

cat > "$STAGE/BUILD-INFO.txt" <<TXT
Product: $PRODUCT
Version: $VERSION
Source commit: $COMMIT
Build: macOS arm64 and x86_64; AU and VST3
Signing: ad-hoc signatures verified; not Developer ID signed or notarized
Windows: not built by this package
Identity: $BUNDLE_ID; manufacturer $MANUFACTURER; plug-in code $PLUGIN_CODE
TXT

rm -f "$ARCHIVE" "$ARCHIVE.sha256"
ditto -c -k --norsrc --keepParent "$STAGE" "$ARCHIVE"
unzip -t "$ARCHIVE" >/dev/null
VERIFY="$DIST/.verify-$NAME"
rm -rf "$VERIFY"
mkdir -p "$VERIFY"
ditto -x -k "$ARCHIVE" "$VERIFY"
EXTRACTED="$VERIFY/$NAME"
for bundle in "$PRODUCT.component" "$PRODUCT.vst3"; do
    binary="$EXTRACTED/$bundle/Contents/MacOS/$PRODUCT"
    test -x "$binary" || { echo "Archived executable missing or not executable: $binary" >&2; exit 1; }
    arches=$(lipo -archs "$binary")
    printf '%s\n' "$arches" | grep -qw arm64 || { echo "Archived binary missing arm64: $binary" >&2; exit 1; }
    printf '%s\n' "$arches" | grep -qw x86_64 || { echo "Archived binary missing x86_64: $binary" >&2; exit 1; }
    codesign --verify --deep --strict "$EXTRACTED/$bundle"
    plist="$EXTRACTED/$bundle/Contents/Info.plist"
    test "$(plutil -extract CFBundleIdentifier raw -o - "$plist")" = "$BUNDLE_ID" || { echo "Archived identity mismatch: $bundle" >&2; exit 1; }
done
for f in THIRD_PARTY_NOTICES.md INSTALL.txt BUILD-INFO.txt README.md licenses/JUCE-LICENSE.md licenses/Inter-LICENSE.txt licenses/JetBrainsMono-LICENSE.txt licenses/VST3-SDK/VST3_License_Agreement.pdf; do
    test -s "$EXTRACTED/$f" || { echo "Archived file missing: $f" >&2; exit 1; }
done
grep -q "^Source commit: $COMMIT\$" "$EXTRACTED/BUILD-INFO.txt" || { echo "BUILD-INFO commit mismatch" >&2; exit 1; }
(cd "$DIST" && shasum -a 256 "$NAME.zip" > "$NAME.zip.sha256")
rm -rf "$VERIFY"
printf 'Created %s\n' "$ARCHIVE"
cat "$ARCHIVE.sha256"
