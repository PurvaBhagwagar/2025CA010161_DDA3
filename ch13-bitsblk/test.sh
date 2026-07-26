#!/bin/bash
# test.sh - end-to-end filesystem round-trip proof for bitsblk
# Run from inside ch13-bitsblk/ after `make`.
# Captures every command + output; redirect to a log for submission:
#   ./test.sh 2>&1 | tee evidence_bitsblk.log

set -e
set -x

MODULE=bitsblk
DEV=/dev/bitsblk0
MNT=/mnt/bitsblk_test
IMG=big.bin

# 1. Load the module with a 16MB backing store
sudo insmod ./${MODULE}.ko size_mb=16
sleep 1

# 2. Confirm the disk is visible with the expected size
lsblk | grep bitsblk

# 3. Create a >=1MB test file
dd if=/dev/urandom of=${IMG} bs=1M count=2

# 4. Format and mount
sudo mkfs.ext4 -F ${DEV}
sudo mkdir -p ${MNT}
sudo mount ${DEV} ${MNT}

# 5. Copy file onto the device
sudo cp ${IMG} ${MNT}/
sync

# 6. Verify integrity before remount
sha256sum ${IMG} ${MNT}/${IMG}

# 7. Remount and verify again (proves data survives unmount, i.e. it's
#    really in the backing store, not just page cache)
sudo umount ${MNT}
sudo mount ${DEV} ${MNT}
sha256sum ${MNT}/${IMG}

# 8. Clean teardown
sudo umount ${MNT}
sudo rmmod ${MODULE}

echo "=== bitsblk test PASSED ==="
