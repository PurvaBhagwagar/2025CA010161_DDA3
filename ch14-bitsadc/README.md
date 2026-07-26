# bitsadc — Emulated IIO ADC with LED Threshold Indicator

**Author:** Purva Bhagwagar (2025CA01061)

## What it is

`bitsadc` is a `platform_driver` matched by name to a `platform_device`
registered from the same module (no device tree required). It exposes
an IIO device with two `IIO_VOLTAGE` channels:

- **Channel 0** — animated by a periodic `hrtimer` (100ms period) as a
  triangle wave across the full 12-bit range 0–4095.
- **Channel 1** — a fixed mid-scale reading (2047).

Both expose `IIO_CHAN_INFO_RAW` and `IIO_CHAN_INFO_SCALE`. Whenever
channel 0 crosses the `threshold` module parameter, an LED classdev
`bits:status:adc` mirrors the comparison at
`/sys/class/leds/bits:status:adc/brightness`.

## Why the hrtimer callback can't sleep, and needs no mutex

**Can't sleep:** `hrtimer_start()` with `HRTIMER_MODE_REL` on
`CLOCK_MONOTONIC` fires the callback from **hardirq/softirq (atomic)
context**, not a process context worker thread. Atomic context has no
valid task to schedule out, so anything that could block — a mutex,
`GFP_KERNEL` allocation, blocking I/O, `msleep()` — is illegal there
and would trip `might_sleep()` debug checks or, worse, deadlock the
timer subsystem. `bitsadc_timer_cb()` only does bounded integer
arithmetic and an LED brightness write, both of which are safe from
atomic context.

**No mutex needed for the sample:** `priv->sample` has exactly one
writer — the timer callback itself, which `hrtimer` guarantees is
never concurrently re-entered on the same timer. The only other
accessor is `read_raw()` on the sysfs read path, and it only *reads*
the value; there's no read-modify-write race to protect against.
Since a properly aligned `int` is loaded/stored atomically by the
hardware on every architecture Linux supports, the only remaining
risk is the *compiler* — it could otherwise cache the value in a
register across iterations, split the access, or reorder it relative
to other code. `WRITE_ONCE()` on the timer's write and `READ_ONCE()`
on the sysfs read close that gap by forcing a single, non-reordered
memory access each time, without the cost or sleep-in-atomic-context
risk of a spinlock/mutex.

## Files

| File | Purpose |
|---|---|
| `bitsadc.c` | The driver |
| `Makefile` | Standard out-of-tree kbuild file |
| `test.sh` | Captures the full sysfs demonstration |

## Build

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
cd ch14-bitsadc
make
```

## Run / demonstrate

```bash
sudo insmod ./bitsadc.ko threshold=3000
ls /sys/bus/iio/devices/                                   # iio:deviceN

cat /sys/bus/iio/devices/iio:device0/in_voltage0_raw        # changes each read
cat /sys/bus/iio/devices/iio:device0/in_voltage1_raw        # fixed
cat /sys/bus/iio/devices/iio:device0/in_voltage0_scale

watch -n 0.2 cat /sys/class/leds/bits:status:adc/brightness # toggles as ch0 crosses 3000

sudo rmmod bitsadc                                          # timer cancelled, no oops
```

Or:

```bash
chmod +x test.sh
./test.sh 2>&1 | tee evidence_bitsadc.log
```

(Replace `iio:device0` with whatever index your system assigns if you
have other IIO devices loaded — `ls /sys/bus/iio/devices/` shows it.)

## Clean unload safety

`bitsadc_remove()` calls `hrtimer_cancel()` **before** returning,
which blocks until any in-flight callback finishes and guarantees no
further callback fires. This must happen before the `devm_*`-managed
IIO device and LED classdev are torn down (which happens automatically
right after `remove()` returns) — otherwise the timer could fire once
more after the LED/IIO structures are freed and dereference freed
memory, oopsing the kernel on `rmmod`.

## Optional bonus (Raspberry Pi + TMP102)

Not implemented in this submission (no Pi hardware available in this
environment). To add it: register a third `IIO_VOLTAGE`... actually
`IIO_TEMP` channel, back it with `i2c_smbus_read_word_swapped()`
against a TMP102 on I2C-1, scale the raw 12-bit code by 0.0625°C, and
add a device-tree overlay with `compatible = "ti,tmp102"` under
`/boot/firmware/overlays/` on Raspberry Pi OS, loaded via
`/boot/firmware/config.txt` `dtoverlay=`.

## Kernel version notes

`platform_driver.remove()` changed from returning `int` to `void` in
Linux 6.11; this file handles both via a small macro guarded by
`LINUX_VERSION_CODE`. Check your kernel with `uname -r` — on a stock
Ubuntu 22.04/24.04 kernel this compiles as-is.
