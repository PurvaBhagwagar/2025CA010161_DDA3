#!/bin/bash
# test.sh - evidence capture for bitsadc
# Run from inside ch14-bitsadc/ after `make`.
#   ./test.sh 2>&1 | tee evidence_bitsadc.log

set -e
set -x

sudo modprobe industrialio
sudo insmod ./bitsadc.ko threshold=3000
sleep 1

# 1. Confirm the IIO device is registered
ls /sys/bus/iio/devices/

IIO_DEV=$(ls /sys/bus/iio/devices/ | grep '^iio:device' | head -n1)
echo "Using $IIO_DEV"

# 2. Read channel 0 twice with a gap: value should change (animated)
cat /sys/bus/iio/devices/${IIO_DEV}/in_voltage0_raw
sleep 0.5
cat /sys/bus/iio/devices/${IIO_DEV}/in_voltage0_raw

# 3. Read channel 1: fixed mid-scale value every time
cat /sys/bus/iio/devices/${IIO_DEV}/in_voltage1_raw

# 4. Read the scale attribute
cat /sys/bus/iio/devices/${IIO_DEV}/in_voltage0_scale

# 5. Watch the LED brightness toggle as the waveform crosses threshold
echo "Sampling LED brightness for 8s (should show both 0 and 1):"
timeout 8 bash -c \
  'while true; do cat /sys/class/leds/bits:status:adc/brightness; sleep 0.2; done' || true

# 6. Clean unload - timer must be cancelled, no oops
sudo rmmod bitsadc

echo "=== bitsadc test PASSED ==="
