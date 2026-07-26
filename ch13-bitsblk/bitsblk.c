// SPDX-License-Identifier: GPL-2.0
/*
 * bitsblk.c - RAM-backed blk-mq block driver
 *
 * Author: Purva Bhagwagar (2025CA01061)
 *
 * A minimal blk-mq block driver whose backing store is a vzalloc()
 * buffer. Exposes /dev/bitsblk0. Supports arbitrary filesystems on
 * top (ext4 demonstrated in test.sh).
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/vmalloc.h>
#include <linux/blk-mq.h>
#include <linux/blkdev.h>
#include <linux/version.h>
#include <linux/ratelimit.h>
#include <linux/errno.h>

#define BITSBLK_NAME         "bitsblk"
#define BITSBLK_DISK_NAME    "bitsblk0"
#define BITSBLK_MINORS       1
#define BITSBLK_QUEUE_DEPTH  64
#define SECTOR_SHIFT         9

static int size_mb = 8;
module_param(size_mb, int, 0444);
MODULE_PARM_DESC(size_mb, "Backing store size in MB (clamped to 4-64, default 8)");

static DEFINE_RATELIMIT_STATE(bitsblk_ratelimit, 5 * HZ, 3);

struct bitsblk_dev {
	struct gendisk        *disk;
	struct blk_mq_tag_set   tag_set;
	u8                     *data;
	size_t                  size_bytes;
	int                     major;
};

static struct bitsblk_dev bblk;

/*
 * Core I/O path: walk every bio_vec in the request and memcpy it
 * to/from the backing store at the right offset. Anything that would
 * run off the end of the backing store completes with BLK_STS_IOERR
 * instead of touching memory out of bounds.
 */
static blk_status_t bitsblk_queue_rq(struct blk_mq_hw_ctx *hctx,
				      const struct blk_mq_queue_data *bd)
{
	struct request *rq = bd->rq;
	struct bio_vec bvec;
	struct req_iterator iter;
	loff_t pos;
	blk_status_t status = BLK_STS_OK;

	blk_mq_start_request(rq);

	pos = (loff_t)blk_rq_pos(rq) << SECTOR_SHIFT;

	rq_for_each_segment(bvec, rq, iter) {
		void *buf;
		size_t len = bvec.bv_len;

		if (pos < 0 || (size_t)pos + len > bblk.size_bytes) {
			status = BLK_STS_IOERR;
			break;
		}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
		buf = kmap_local_page(bvec.bv_page) + bvec.bv_offset;
#else
		buf = kmap_atomic(bvec.bv_page) + bvec.bv_offset;
#endif

		if (rq_data_dir(rq) == WRITE)
			memcpy(bblk.data + pos, buf, len);
		else
			memcpy(buf, bblk.data + pos, len);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
		kunmap_local(buf);
#else
		kunmap_atomic(buf);
#endif
		pos += len;
	}

	blk_mq_end_request(rq, status);
	return BLK_STS_OK;
}

static const struct blk_mq_ops bitsblk_mq_ops = {
	.queue_rq = bitsblk_queue_rq,
};

/*
 * block_device_operations.open()/.release() signatures changed
 * across kernel releases; the #if branches keep this file buildable
 * on both pre- and post-6.5 kernels without edits.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
static int bitsblk_open(struct gendisk *disk, blk_mode_t mode)
{
	return 0;
}

static void bitsblk_release(struct gendisk *disk)
{
}
#else
static int bitsblk_open(struct block_device *bdev, fmode_t mode)
{
	return 0;
}

static void bitsblk_release(struct gendisk *disk, fmode_t mode)
{
}
#endif

static const struct block_device_operations bitsblk_fops = {
	.owner   = THIS_MODULE,
	.open    = bitsblk_open,
	.release = bitsblk_release,
};

static int __init bitsblk_init(void)
{
	int ret;
	sector_t sectors;

	if (size_mb < 4 || size_mb > 64) {
		if (__ratelimit(&bitsblk_ratelimit))
			pr_warn("bitsblk: size_mb=%d out of range [4,64], clamping\n",
				size_mb);
		size_mb = clamp(size_mb, 4, 64);
	}

	bblk.size_bytes = (size_t)size_mb * 1024 * 1024;

	/*
	 * vzalloc(), not kzalloc(): kzalloc()/the page allocator only
	 * hands out *physically contiguous* memory, capped at
	 * MAX_ORDER (typically 4MB on a 4K-page x86 system, i.e.
	 * order-10). An 8-64MB request is at or beyond that ceiling
	 * and would either fail outright or force an unreasonably
	 * high allocation order that fragments the buddy allocator.
	 * vzalloc() only needs *virtually* contiguous pages, which
	 * vmalloc's page-table remapping happily assembles from
	 * scattered physical pages, and it zero-fills the region for
	 * us (matters here since stale kernel memory must never be
	 * readable through the block device before it's written).
	 * The one-time TLB/page-table setup cost of vmalloc is
	 * irrelevant for a slow-path RAM disk backing store that is
	 * allocated once at module load and accessed by plain
	 * memcpy(), not DMA.
	 */
	bblk.data = vzalloc(bblk.size_bytes);
	if (!bblk.data)
		return -ENOMEM;

	memset(&bblk.tag_set, 0, sizeof(bblk.tag_set));
	bblk.tag_set.ops = &bitsblk_mq_ops;
	bblk.tag_set.nr_hw_queues = 1;
	bblk.tag_set.queue_depth = BITSBLK_QUEUE_DEPTH;
	bblk.tag_set.numa_node = NUMA_NO_NODE;
	bblk.tag_set.flags = BLK_MQ_F_SHOULD_MERGE;

	ret = blk_mq_alloc_tag_set(&bblk.tag_set);
	if (ret)
		goto out_free_mem;

	bblk.major = register_blkdev(0, BITSBLK_NAME);
	if (bblk.major < 0) {
		ret = bblk.major;
		goto out_free_tags;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 18, 0)
	bblk.disk = blk_mq_alloc_disk(&bblk.tag_set, NULL);
#else
	bblk.disk = blk_mq_alloc_disk(&bblk.tag_set, NULL);
#endif
	if (IS_ERR(bblk.disk)) {
		ret = PTR_ERR(bblk.disk);
		goto out_unregister;
	}

	sectors = bblk.size_bytes >> SECTOR_SHIFT;

	bblk.disk->major = bblk.major;
	bblk.disk->first_minor = 0;
	bblk.disk->minors = BITSBLK_MINORS;
	bblk.disk->fops = &bitsblk_fops;
	bblk.disk->private_data = &bblk;
	snprintf(bblk.disk->disk_name, DISK_NAME_LEN, BITSBLK_DISK_NAME);
	set_capacity(bblk.disk, sectors);

	ret = add_disk(bblk.disk);
	if (ret)
		goto out_cleanup_disk;

	pr_info("bitsblk: %s ready, %d MB (%llu sectors)\n",
		BITSBLK_DISK_NAME, size_mb, (unsigned long long)sectors);
	return 0;

out_cleanup_disk:
	put_disk(bblk.disk);
out_unregister:
	unregister_blkdev(bblk.major, BITSBLK_NAME);
out_free_tags:
	blk_mq_free_tag_set(&bblk.tag_set);
out_free_mem:
	vfree(bblk.data);
	return ret;
}

static void __exit bitsblk_exit(void)
{
	del_gendisk(bblk.disk);
	put_disk(bblk.disk);
	unregister_blkdev(bblk.major, BITSBLK_NAME);
	blk_mq_free_tag_set(&bblk.tag_set);
	vfree(bblk.data);
	pr_info("bitsblk: unloaded cleanly\n");
}

module_init(bitsblk_init);
module_exit(bitsblk_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Purva Bhagwagar <2025CA01061>");
MODULE_DESCRIPTION("RAM-backed blk-mq block driver (bitsblk)");
