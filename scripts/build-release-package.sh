#!/bin/sh

# TODO: Change this
PROJECT_NAME=cleverplant-fw

set -e

rev="${FW_REV:-${CI_COMMIT_TAG:-}}";
[ -n "$rev" ] || rev=$(git describe --dirty 2> /dev/null)
rev=${rev:-v0.0.0}
rev=v${rev#v}
[ -z "$rev" ] && exit 1

rm -rf "release/$rev" || true
mkdir -p "release/$rev"

git archive HEAD -o release/$rev/$PROJECT_NAME-firmware-src.$rev.zip

export BUILD_TYPE=Release
export BUILD_DIR=build/release-$rev
rm -rf "$BUILD_DIR" || true

just build -p

cp $BUILD_DIR/app/zephyr/zephyr.signed.bin release/$rev/$PROJECT_NAME-firmware-dfu.$rev.bin
cp $BUILD_DIR/app/zephyr/zephyr.signed.hex release/$rev/$PROJECT_NAME-firmware.$rev.hex
cp $BUILD_DIR/mcuboot/zephyr/zephyr.hex release/$rev/$PROJECT_NAME-bootloader.$rev.hex

cp changelog.md release/$rev/$PROJECT_NAME-firmware-changelog.$rev.txt

# TODO: update the below documentation
cat > release/$rev/README.txt <<EOF
# CleverPlant Firmware Package

This folder contains all of the required files and documentation for a $PROJECT_NAME firmware release.

- $PROJECT_NAME-firmware-src.vX.X.X.zip: The source code for the firmware and host tools
- $PROJECT_NAME-firmware-dfu.vX.X.X.bin: The compiled firmware binary, used for firmware updates through the serial bootloader
- $PROJECT_NAME-firmware-changelog-vX.X.X.pdf: Changelog for this and prior release versions
- $PROJECT_NAME-firmware.vX.X.X.hex: Compiled firmware binary in hex format (for use in production).
- $PROJECT_NAME-bootloader.vX.X.X.hex: Compiled bootloader binary in hex format (for use in production).

Developed by
Move Innovation ApS
info@moveinnovation.dk

EOF

echo "=== Generated release package in release/$rev ==="
echo "=!= Please verify the contents and update the build-release.sh script if needed =!="
