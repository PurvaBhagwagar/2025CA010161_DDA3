# bitsblk — RAM-Backed blk-mq Block Driver

**Author:** Purva Bhagwagar (2025CA01061)

## What it is

`bitsblk` registers a single `blk_mq` block device, `/dev/bitsblk0`,
backed by a plain kernel-memory buffer. It behaves like a real disk to
any filesystem placed on top of it (demonstrated here with ext4), but
all storage lives in RAM and disappears on module unload.

## Why `vzalloc()` and not `kzalloc()`

`kzalloc()` (and the page allocator underneath it) can only return
**physically contiguous** memory, and that memory is capped by
`MAX_ORDER` — on a typical x86_64 system with 4K pages this ceiling
sits around a few MB (order-10, i.e. ~4MB) before allocation starts
failing or requires unreasonably large, fragmentation-prone requests.
Our backing store defaults to 8MB and can go up to 64MB, which is at
or beyond that ceiling.

`vzalloc()` only needs the memory to be **virtually contiguous** — it
stitches together scattered physical pages via the kernel's page
tables. That's exactly what a block driver needs: `queue_rq()` only
ever touches this buffer with `memcpy()`, walking it as one flat
address range; it never needs the memory to be physically contiguous
because there's no DMA involved. `vzalloc()` also zero-fills the
region, which matters here — without it, a freshly loaded module
would expose whatever stale physical memory the allocator handed it
to any process reading an un-written sector.

The one-time cost of `vzalloc()` (setting up page-table mappings) is
paid once at `insmod` time and is irrelevant next to the per-I/O cost
of the driver's own `memcpy()` path.

## Files

| File | Purpose |
|---|---|
| `bitsblk.c` | The driver |
| `Makefile` | Standard out-of-tree kbuild file |
| `test.sh` | Full filesystem round-trip proof (see assignment demo) |

## Build

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
cd ch13-bitsblk
make
```

This produces `bitsblk.ko`.

## Run / demonstrate

```bash
sudo insmod ./bitsblk.ko size_mb=16
lsblk | grep bitsblk0                       # 16M disk visible
sudo mkfs.ext4 /dev/bitsblk0
sudo mkdir -p /mnt/bitsblk_test
sudo mount /dev/bitsblk0 /mnt/bitsblk_test
dd if=/dev/urandom of=big.bin bs=1M count=2
sudo cp big.bin /mnt/bitsblk_test/
sha256sum big.bin /mnt/bitsblk_test/big.bin  # identical
sudo umount /mnt/bitsblk_test
sudo mount /dev/bitsblk0 /mnt/bitsblk_test
sha256sum /mnt/bitsblk_test/big.bin          # still identical
sudo umount /mnt/bitsblk_test
sudo rmmod bitsblk                           # clean teardown, no oops
```

Or simply:

```bash
chmod +x test.sh
./test.sh 2>&1 | tee evidence_bitsblk.log
```

## Out-of-bounds behavior

Any request whose segment would read/write past `size_mb` MB
completes with `BLK_STS_IOERR` instead of touching memory outside the
`vzalloc()` region — verified implicitly by the fact that `mkfs.ext4`
and the file copy above only ever address sectors within capacity;
you can additionally test this by trying to `dd` directly past the
end of the device (`dd if=/dev/zero of=/dev/bitsblk0 bs=1M seek=100
count=1`), which returns an I/O error rather than crashing the
kernel.

## Kernel version notes

Written against modern kernels using `blk_mq_alloc_disk()` (5.18+)
and the post-6.5 `block_device_operations.open(gendisk*, blk_mode_t)`
signature, with `#if LINUX_VERSION_CODE` guards for older kernels.
Check your kernel with `uname -r` before building; on a stock Ubuntu
22.04/24.04 kernel this compiles as-is.
