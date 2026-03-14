#!/bin/sh
cat > artifacts/README.txt << EOF
artifacts
$(for board in $(ls -d project/boards/*/ 2>/dev/null | xargs -n1 basename | sort); do
echo "├── fw_${CI_COMMIT_TAG}_hw_${board}_fota.bin"
echo "├── fw_${CI_COMMIT_TAG}_hw_${board}_jig.hex"
done)
├── mcuboot.hex
└── README.txt

File descriptions:

1. fw_<version>_hw_<board>_jig.hex
   Production flashing file for the jig. Upload to the production jig.
   - fw = firmware version (e.g., ${CI_COMMIT_TAG})
   - hw = hardware version (e.g., C3, C5)

2. fw_<version>_hw_<board>_fota.bin
   FOTA update file for over-the-air updates.

3. mcuboot.hex
   The bootloader. Shared across all hardware versions.
EOF
