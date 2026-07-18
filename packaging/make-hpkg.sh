#!/bin/sh
# make-hpkg.sh — build the `lv2_haiku` .hpkg on a running Haiku (x86_64):
# the lv2jackhost binary, the lilv tools, the LV2 spec bundles and the hkamp
# example plugin. Requires jack (libjack) to be installed first (lv2jackhost
# links -ljack).
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
VERSION=0.2.0
REVISION=1
STAGE_PKG="$HERE/stage"

pkgman install -y meson ninja gcc make pkgconfig || true

cd "$ROOT"
rm -rf build "$STAGE_PKG"
make

mkdir -p "$STAGE_PKG/bin" "$STAGE_PKG/add-ons/lv2"

cp build/bin/lv2jackhost "$STAGE_PKG/bin/"
for tool in lv2ls lv2info lv2apply; do
    cp "build/stage/bin/$tool" "$STAGE_PKG/bin/"
done

# LV2 spec bundles (lilv needs them on LV2_PATH) + the example plugin.
for b in build/stage/lib/lv2/*.lv2; do
    [ -d "$b" ] || continue
    cp -r "$b" "$STAGE_PKG/add-ons/lv2/"
done
cp -r build/hkamp.lv2 "$STAGE_PKG/add-ons/lv2/"

cp "$HERE/lv2_haiku.PackageInfo" "$STAGE_PKG/.PackageInfo"
OUT="$HERE/lv2_haiku-$VERSION-$REVISION-x86_64.hpkg"
package create -C "$STAGE_PKG" "$OUT"
echo ">> built $OUT"
