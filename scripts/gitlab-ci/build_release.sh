#!/bin/sh
set -e

mkdir -p artifacts

for board_dir in project/boards/*/; do
    board=$(basename "$board_dir")
    echo "Building for board: $board"

    BOARD="$board/nrf5340/cpuapp" just release-build -p

    cp "build/$board/nrf5340/cpuapp/app/zephyr/zephyr.signed.hex" "artifacts/fw_${CI_COMMIT_TAG:-local}_hw_${board}_jig.hex"
    cp "build/$board/nrf5340/cpuapp/app/zephyr/zephyr.signed.bin" "artifacts/fw_${CI_COMMIT_TAG:-local}_hw_${board}_fota.bin"
    cp "build/$board/nrf5340/cpuapp/mcuboot/zephyr/zephyr.hex" "artifacts/mcuboot.hex"
done

echo "Build complete. Artifacts:"
ls -la artifacts/
