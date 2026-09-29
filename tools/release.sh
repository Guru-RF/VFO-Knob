#!/usr/bin/env bash
# Build a signed image and stage it for OTA.
#
# Firmware is published under firmware/ and fetched over
# raw.githubusercontent.com rather than as a GitHub release asset. That is not
# a preference: release assets redirect to release-assets.githubusercontent.com,
# which sends no Access-Control-Allow-Origin, and the configuration page has to
# be able to fetch the image itself. Over the USB cable the knob is the DHCP
# server on a link with no gateway, so it cannot reach GitHub at all and the
# browser must do the downloading. raw.githubusercontent.com sends "*".
#
#   tools/release.sh 1.2.3            stage locally
#   tools/release.sh 1.2.3 --push     stage and push the firmware branch
#
# The firmware branch is kept ORPHAN so 1.7 MB per release never lands in the
# history of main.
set -euo pipefail

VER="${1:-}"
[ -n "$VER" ] || { echo "usage: $0 <version> [--push]" >&2; exit 1; }
[[ "$VER" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "version must be x.y.z" >&2; exit 1; }
PUSH="${2:-}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

[ -f ota_signing_key.pem ] || {
    echo "ota_signing_key.pem is missing -- without it nothing already in the" >&2
    echo "field can ever be updated again. Restore it from your backup." >&2
    exit 1; }

# The image takes its version from `git describe`, so the release commit must
# be tagged and the tree clean. An image that calls itself anything else is
# installed, still reads as older than the manifest, and is offered again on
# every check, forever.
DESC=$(git describe --tags --dirty --always)
[ "$DESC" = "v$VER" ] || {
    echo "HEAD describes as $DESC, not a clean v$VER -- tag it first" >&2
    exit 1; }

# sdkconfig.defaults.local is optional, as in CMakeLists.txt. IDF refuses a
# defaults file that does not exist, and a machine that only cuts releases
# has no business holding anyone's WiFi password.
DEFAULTS="sdkconfig.defaults"
[ -f sdkconfig.defaults.local ] && DEFAULTS="$DEFAULTS;sdkconfig.defaults.local"

echo "==> building $VER"
# reconfigure: CMake reads `git describe` only when it configures, and a new
# tag on an already-built commit does not make it configure again.
idf.py -B build_usbnet -D SDKCONFIG_DEFAULTS="$DEFAULTS;sdkconfig.usbnet" \
       reconfigure build >/dev/null

BIN="build_usbnet/vfo-knob.bin"
# esp_app_desc_t leads the first segment: magic at byte 32, version at 48.
IMGVER=$(python3 -c 'import sys; b = open(sys.argv[1], "rb").read()
print(b[48:80].split(b"\0")[0].decode() if b[32:36] == bytes.fromhex("3254cdab") else "?")' "$BIN")
[ "$IMGVER" = "v$VER" ] || {
    echo "the image says it is $IMGVER, not v$VER -- refusing to publish" >&2
    exit 1; }

echo "==> verifying the signature before publishing it"
espsecure.py verify_signature --version 2 --keyfile ota_signing_key.pem "$BIN" \
    | grep -q "verification successful" \
    || { echo "image is not signed with this key -- refusing to publish" >&2; exit 1; }

mkdir -p firmware
OUT="vfo-knob-$VER.bin"
cp "$BIN" "firmware/$OUT"
SHA=$(sha256sum "firmware/$OUT" | cut -d' ' -f1)
SIZE=$(stat -c%s "firmware/$OUT")

cat > firmware/manifest.json <<JSON
{
  "version": "$VER",
  "file": "$OUT",
  "sha256": "$SHA",
  "size": $SIZE
}
JSON

echo "==> staged firmware/$OUT ($((SIZE/1024)) KB)"
cat firmware/manifest.json

[ "$PUSH" = "--push" ] || { echo "==> not pushed (pass --push)"; exit 0; }

echo "==> publishing to the firmware branch"
TMP=$(mktemp -d)
cp firmware/manifest.json "firmware/$OUT" "$TMP/"
git fetch origin firmware:firmware 2>/dev/null || true
if git rev-parse --verify firmware >/dev/null 2>&1; then
    git worktree add "$TMP/wt" firmware >/dev/null
else
    git worktree add --detach "$TMP/wt" >/dev/null
    git -C "$TMP/wt" checkout --orphan firmware >/dev/null
    git -C "$TMP/wt" rm -rf . >/dev/null 2>&1 || true
fi
mkdir -p "$TMP/wt/firmware"
cp "$TMP/manifest.json" "$TMP/$OUT" "$TMP/wt/firmware/"
git -C "$TMP/wt" add firmware
git -C "$TMP/wt" commit -q -m "firmware $VER"
git -C "$TMP/wt" push -u origin firmware
git worktree remove --force "$TMP/wt"
rm -rf "$TMP"
echo "==> published; devices will see $VER within their check interval"
