#!/bin/sh
# One-shot build + install ON A HAIKU MACHINE (run this inside Haiku, not on
# the dev host). Mirrors the sibling ports' build-from-source scripts.
#
#   ./build-from-source.sh
#
# Installs the LV2 spec bundles and hkamp.lv2 into the user's non-packaged
# add-ons dir and the lilv tools into /boot/system/non-packaged/bin.

set -eu

pkgman install -y meson ninja pkgconfig gcc make

make
make install-specs install

mkdir -p /boot/system/non-packaged/bin
for tool in lv2ls lv2info lv2apply; do
    cp build/stage/bin/$tool /boot/system/non-packaged/bin/
done
cp build/bin/lv2jackhost /boot/system/non-packaged/bin/

echo "done: stack in build/stage, tools in /boot/system/non-packaged/bin,"
echo "bundles in $HOME/config/non-packaged/add-ons/lv2"
