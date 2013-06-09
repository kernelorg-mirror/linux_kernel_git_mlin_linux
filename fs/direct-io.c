/*
 * fs/direct-io.c
 *
 * Copyright (C) 2002, Linus Torvalds.
 *
 * O_DIRECT
 *
 * 04Jul2002	Andrew Morton
 *		Initial version
 * 11Sep2002	janetinc@us.ibm.com
 *		added readv/writev support.
 * 29Oct2002	Andrew Morton
 *		rewrote bio_add_page() support.
 * 30Oct2002	pbadari@us.ibm.com
 *		added support for non-aligned IO.
 * 06Nov2002	pbadari@us.ibm.com
 *		added asynchronous IO support.
 * 21Jul2003	nathans@sgi.com
 *		added IO completion notifier.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/highmem.h>
#include <linux/pagemap.h>
#include <linux/task_io_accounting_ops.h>
#include <linux/bio.h>
#include <linux/wait.h>
#include <linux/err.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/rwsem.h>
#include <linux/uio.h>
#include <linux/atomic.h>
#include <linux/prefetch.h>
#include <linux/aio.h>

/* dio_state only used in the submission path */
struct dio_submit {
	get_block_t	*get_block;	/* block mapping function */
	dio_submit_t	*submit_io;	/* IO submition function */
	unsigned	i_blkbits;
};

/* dio_state communicated between submission path and end_io */
struct dio {
	int		flags;		/* doesn't change */
	int		rw;
	struct inode	*inode;
	loff_t		i_size;		/* i_size when submitted */

	dio_iodone_t	*end_io;	/* IO completion function */
	void		*private;	/* copy from map_bh.b_private */

	/* BIO completion state */
	int		page_error;	/* errno from get_user_pages() */
	int		io_error;	/* IO error in completion path */
	atomic_long_t	refcount;	/* direct_io_worker() and bios */
	struct task_struct *waiter;	/* waiting task (NULL if none) */

	/* AIO related stuff */
	struct kiocb	*iocb;		/* kiocb */
	ssize_t		result;		/* IO result */

	struct bio	bio;
};

static struct bio_set *dio_pool __read_mostly;

/**
 * dio_complete() - called when all DIO BIO I/O has been completed
 * @offset: the byte offset in the file of the completed operation
 *
 * This releases locks as dictated by the locking type, lets interested parties
 * know that a DIO operation has completed, and calculates the resulting return
 * code for the operation.
 *
 * It lets the filesystem know if it registered an interest earlier via
 * get_block.  Pass the private field of the map buffer_head so that
 * filesystems can use it to hold additional state between get_block calls and
 * dio_complete.
 */
static ssize_t dio_complete(struct dio *dio, loff_t offset, ssize_t ret, bool is_async)
{
	ssize_t transferred = 0;

	if (dio->result) {
		transferred = dio->result;

		/* XXX: dio_send_bio() could do this */

		/* Check for short read case */
		if ((dio->rw == READ) && ((offset + transferred) > dio->i_size))
			transferred = dio->i_size - offset;
	}

	if (ret == 0)
		ret = dio->page_error;
	if (ret == 0)
		ret = dio->io_error;
	if (ret == 0)
		ret = transferred;

	if (dio->end_io && dio->result) {
		dio->end_io(dio->iocb, offset, transferred,
			    dio->private, ret, is_async);
	} else {
		inode_dio_done(dio->inode);
		if (is_async)
			aio_complete(dio->iocb, ret, 0);
	}

	bio_put(&dio->bio);
	return ret;
}

#define DIO_WAKEUP	(1U << 31)

/**
 * dio_end_io - handle the end io action for the given bio
 * @bio: The direct io bio thats being completed
 * @error: Error if there was one
 *
 * This is meant to be called by any filesystem that uses their own dio_submit_t
 * so that the DIO specific endio actions are dealt with after the filesystem
 * has done it's completion work.
 */
void dio_end_io(struct bio *bio, int error)
{
	struct dio *dio = bio->bi_private;
	unsigned long remaining;

	if (error)
		dio->io_error = -EIO;

	if (dio->rw == READ) {
		bio_check_pages_dirty(bio);	/* transfers ownership */
	} else {
		struct bio_vec *bv;
		int i;

		bio_for_each_segment_all(bv, bio, i)
			page_cache_release(bv->bv_page);
		bio_put(bio);
	}

	remaining = atomic_long_dec_return(&dio->refcount);

	if (remaining == DIO_WAKEUP)
		wake_up_process(dio->waiter);
	else if (!remaining)
		dio_complete(dio, dio->iocb->ki_pos, 0, true);
}
EXPORT_SYMBOL_GPL(dio_end_io);

static void dio_wait_completion(struct dio *dio)
{
	if (atomic_long_add_return(DIO_WAKEUP - 1,
				   &dio->refcount) == DIO_WAKEUP)
		return;

	while (1) {
		set_current_state(TASK_UNINTERRUPTIBLE);
		if (atomic_long_read(&dio->refcount) == DIO_WAKEUP)
			break;

		io_schedule();
	}
	__set_current_state(TASK_RUNNING);
}

/*
 * For reads we speculatively dirty the pages before starting IO. During IO
 * completion, any of these pages which happen to have been written back will be
 * redirtied by bio_check_pages_dirty().
 *
 * bios hold a dio reference between submit_bio and ->end_io.
 */
static void dio_bio_submit(struct dio *dio, struct dio_submit *sdio,
			   struct bio *bio, loff_t offset)
{
	/*
	 * Read accounting is performed in submit_bio()
	 */
	if (dio->rw & WRITE)
		task_io_account_write(bio->bi_iter.bi_size);

	if (sdio->submit_io)
		sdio->submit_io(dio->rw, bio, dio->inode,
				offset >> sdio->i_blkbits);
	else
		submit_bio(dio->rw, bio);
}

/*
 * Clean any dirty buffers in the blockdev mapping which alias newly-created
 * file blocks.  Only called for S_ISREG files - blockdevs do not set buffer_new
 */
static void clean_blockdev_aliases(struct dio *dio, struct dio_submit *sdio,
				   struct buffer_head *map_bh)
{
	unsigned i;
	unsigned nblocks;

	nblocks = map_bh->b_size >> sdio->i_blkbits;

	for (i = 0; i < nblocks; i++)
		unmap_underlying_metadata(map_bh->b_bdev,
					  map_bh->b_blocknr + i);
}

struct dio_mapping {
	enum {
		MAP_MAPPED,
		MAP_NEW,
		MAP_UNMAPPED,
	} state;

	struct block_device	*bdev;
	sector_t		sector;
	size_t			size;
};

static int get_blocks(struct dio *dio, struct dio_submit *sdio,
		      loff_t offset, size_t size,
		      struct dio_mapping *map)
{
	struct buffer_head map_bh = { 0, };
	int ret, create;
	unsigned i_mask = (1 << sdio->i_blkbits) - 1;
	unsigned fs_offset = offset & i_mask;
	sector_t fs_block = offset >> sdio->i_blkbits;

	/*
	 * For writes inside i_size on a DIO_SKIP_HOLES filesystem we
	 * forbid block creations: only overwrites are permitted.
	 * We will return early to the caller once we see an
	 * unmapped buffer head returned, and the caller will fall
	 * back to buffered I/O.
	 *
	 * Otherwise the decision is left to the get_blocks method,
	 * which may decide to handle it or also return an unmapped
	 * buffer head.
	 */
	create = dio->rw & WRITE;
	if (dio->flags & DIO_SKIP_HOLES) {
		if (fs_block < dio->i_size >> sdio->i_blkbits)
			create = 0;
	}

	map_bh.b_state = 0;
	map_bh.b_size = size + fs_offset;

	ret = sdio->get_block(dio->inode, fs_block,
			      &map_bh, create);
	if (ret)
		return ret;

	/* Store for completion */
	dio->private = map_bh.b_private;

	if (!buffer_mapped(&map_bh))
		map->state = MAP_UNMAPPED;
	else if (buffer_new(&map_bh))
		map->state = MAP_NEW;
	else
		map->state = MAP_MAPPED;

	/* Holes always 1 block? */
	if (map->state == MAP_UNMAPPED)
		map_bh.b_size = 1 << sdio->i_blkbits;

	if (map->state == MAP_NEW)
		clean_blockdev_aliases(dio, sdio, &map_bh);

	BUG_ON(map_bh.b_size <= fs_offset);

	map->bdev = map_bh.b_bdev;
	map->sector = (map_bh.b_blocknr << (sdio->i_blkbits - 9)) +
		(fs_offset >> 9);
	map->size = min(map_bh.b_size - fs_offset, size);

	return ret;
}

static void dio_write_zeroes(struct dio *dio, struct bio *parent,
			     sector_t sector, size_t size)
{
	unsigned pages = DIV_ROUND_UP(size, PAGE_SIZE);
	struct bio *bio = bio_alloc(GFP_KERNEL, pages);

	while (pages--) {
		bio->bi_io_vec[pages].bv_page = ZERO_PAGE(0);
		bio->bi_io_vec[pages].bv_len = PAGE_SIZE;
		bio->bi_io_vec[pages].bv_offset = 0;
	}

	bio->bi_bdev = parent->bi_bdev;
	bio->bi_iter.bi_sector = sector;
	bio->bi_iter.bi_size = size;

	bio_chain(bio, parent);
	submit_bio(WRITE, bio);
}

static void dio_zero_partial_block(struct dio *dio, struct dio_submit *sdio,
				   struct bio *bio, loff_t offset,
				   struct dio_mapping *map)
{
	if ((dio->rw & WRITE) && map->state == MAP_NEW) {
		unsigned blksize = 1 << sdio->i_blkbits;
		unsigned blkmask = blksize - 1;
		unsigned front = offset & blkmask;
		unsigned back = (offset + bio->bi_iter.bi_size) & blkmask;

		if (front)
			dio_write_zeroes(dio, bio,
					 bio->bi_iter.bi_sector - (front >> 9),
					 front);

		if (back)
			dio_write_zeroes(dio, bio, bio_end_sector(bio),
					 blksize - back);
	}
}

static int dio_send_bio(struct dio *dio, struct dio_submit *sdio,
			struct bio *bio, loff_t offset)
{
	struct dio_mapping map;
	struct bio *split;
	int ret;

	while (1) {
		if (dio->rw == READ && offset >= dio->i_size)
			break;

		ret = get_blocks(dio, sdio, offset, bio->bi_iter.bi_size, &map);
		if (ret)
			break;

		if (map.state != MAP_UNMAPPED) {
			split = bio_next_split(bio, map.size >> 9,
					       GFP_KERNEL, fs_bio_set);

			if (split != bio)
				bio_chain(split, bio);

			split->bi_bdev = map.bdev;
			split->bi_iter.bi_sector = map.sector;

			dio_zero_partial_block(dio, sdio, split, offset, &map);

			dio->result += map.size;
			dio_bio_submit(dio, sdio, split, offset);

			if (split == bio)
				return 0;
		} else {
			/* Hole */

			/* AKPM: eargh, -ENOTBLK is a hack */
			if (dio->rw & WRITE) {
				ret = -ENOTBLK;
				break;
			}

			swap(bio->bi_iter.bi_size, map.size);
			zero_fill_bio(bio);
			swap(bio->bi_iter.bi_size, map.size);

			dio->result += map.size;
			bio_advance(bio, map.size);

			if (!bio->bi_iter.bi_size)
				break;
		}

		offset += map.size;
	}

	bio_endio(bio, 0);
	return ret;
}

static int dio_alloc_bios(struct dio *dio, struct dio_submit *sdio,
			  const struct iovec *iov, loff_t offset,
			  unsigned long nr_segs, unsigned nr_pages)
{
	ssize_t ret;
	size_t seg_done = 0;
	unsigned seg = 0;
	struct bio *bio;

	bio = &dio->bio;
	bio_get(bio);
	goto start;

	while (seg < nr_segs) {
		BUG_ON(!nr_pages);

		bio = bio_alloc(GFP_KERNEL,
				min_t(unsigned, BIO_MAX_PAGES, nr_pages));
start:
		bio->bi_private = dio;
		bio->bi_end_io = dio_end_io;

		while (bio->bi_vcnt < bio->bi_max_vecs &&
		       seg < nr_segs) {
			ret = bio_get_user_pages(bio,
					(size_t) iov[seg].iov_base + seg_done,
					iov[seg].iov_len - seg_done,
					dio->rw == READ);
			if (ret < 0) {
				struct bio_vec *bv;
				int i;

				bio_for_each_segment_all(bv, bio, i)
					page_cache_release(bv->bv_page);
				bio_put(bio);

				dio->page_error = ret;
				return 0;
			}

			seg_done += ret;

			if (seg_done == iov[seg].iov_len) {
				seg++;
				seg_done = 0;
			}
		}

		nr_pages -= bio->bi_vcnt;

		if (dio->rw == READ)
			bio_set_pages_dirty(bio);

		atomic_long_inc(&dio->refcount);
		ret = dio_send_bio(dio, sdio, bio, offset + dio->result);
		if (ret)
			return ret;
	}

	return 0;
}

/*
 * This is a library function for use by filesystem drivers.
 *
 * The locking rules are governed by the flags parameter:
 *  - if the flags value contains DIO_LOCKING we use a fancy locking
 *    scheme for dumb filesystems.
 *    For writes this function is called under i_mutex and returns with
 *    i_mutex held, for reads, i_mutex is not held on entry, but it is
 *    taken and dropped again before returning.
 *  - if the flags value does NOT contain DIO_LOCKING we don't use any
 *    internal locking but rather rely on the filesystem to synchronize
 *    direct I/O reads/writes versus each other and truncate.
 *
 * To help with locking against truncate we incremented the i_dio_count
 * counter before starting direct I/O, and decrement it once we are done.
 * Truncate can wait for it to reach zero to provide exclusion.  It is
 * expected that filesystem provide exclusion between new direct I/O
 * and truncates.  For DIO_LOCKING filesystems this is done by i_mutex,
 * but other filesystems need to take care of this on their own.
 *
 * NOTE: if you pass "sdio" to anything by pointer make sure that function
 * is always inlined. Otherwise gcc is unable to split the structure into
 * individual fields and will generate much worse code. This is important
 * for the whole file.
 */
static ssize_t
do_blockdev_direct_IO(int rw, struct kiocb *iocb, struct inode *inode,
	struct block_device *bdev, const struct iovec *iov, loff_t offset,
	unsigned long nr_segs, get_block_t get_block, dio_iodone_t end_io,
	dio_submit_t submit_io,	int flags)
{
	unsigned nr_pages = 0, blocksize_mask;
	size_t size = 0;
	ssize_t retval = 0;
	const struct iovec *v;
	struct dio *dio;
	struct dio_submit sdio;
	struct blk_plug plug;

	if (rw & WRITE)
		rw = WRITE_ODIRECT;

	sdio.get_block	= get_block;
	sdio.submit_io	= submit_io;
	sdio.i_blkbits	= ACCESS_ONCE(inode->i_blkbits);

	for (v = iov; v < iov + nr_segs; v++) {
		unsigned offset = (size_t) v->iov_base & (PAGE_SIZE - 1);

		nr_pages += DIV_ROUND_UP(offset + v->iov_len, PAGE_SIZE);
		size += v->iov_len;
	}

	/* watch out for a 0 len io from a tricksy fs */
	if (rw == READ && !size)
		return 0;

	blocksize_mask = (1 << sdio.i_blkbits) - 1;

	/*
	 * Avoid references to bdev if not absolutely needed to give
	 * the early prefetch in the caller enough time.
	 */

	if (unlikely((offset & blocksize_mask) ||
		     (size & blocksize_mask))) {
		if (bdev)
			blocksize_mask = roundup_pow_of_two(
				bdev_logical_block_size(bdev)) - 1;

		if ((offset & blocksize_mask) ||
		    (size & blocksize_mask))
			return -EINVAL;
	}

	if (flags & DIO_LOCKING) {
		if (rw == READ) {
			struct address_space *mapping =
					iocb->ki_filp->f_mapping;

			/* will be released by direct_io_worker */
			mutex_lock(&inode->i_mutex);

			retval = filemap_write_and_wait_range(mapping, offset,
							   offset + size - 1);
			if (retval) {
				mutex_unlock(&inode->i_mutex);
				return retval;
			}
		}
	}

	/*
	 * Will be decremented at I/O completion time.
	 */
	atomic_inc(&inode->i_dio_count);

	dio = container_of(bio_alloc_bioset(GFP_KERNEL,
				    min_t(unsigned, BIO_MAX_PAGES, nr_pages),
				    dio_pool),
			   struct dio, bio);

	dio->flags	= flags;
	dio->rw		= rw;
	dio->inode	= inode;
	dio->i_size	= i_size_read(inode);
	dio->end_io	= end_io;
	dio->private	= NULL;
	dio->page_error	= 0;
	dio->io_error	= 0;
	atomic_long_set(&dio->refcount, 1);
	dio->waiter	= current;
	dio->iocb	= iocb;
	dio->result	= 0;

	blk_start_plug(&plug);

	retval = dio_alloc_bios(dio, &sdio, iov, offset, nr_segs, nr_pages);

	if (retval == -ENOTBLK) {
		/*
		 * The remaining part of the request will be
		 * be handled by buffered I/O when we return
		 */
		retval = 0;
	}

	blk_finish_plug(&plug);

	/*
	 * All block lookups have been performed. For READ requests
	 * we can let i_mutex go now that its achieved its purpose
	 * of protecting us from looking up uninitialized blocks.
	 */
	if (rw == READ && (dio->flags & DIO_LOCKING))
		mutex_unlock(&dio->inode->i_mutex);

	/*
	 * The only time we want to leave bios in flight is when a successful
	 * partial aio read or full aio write have been setup.  In that case
	 * bio completion will call aio_complete.  The only time it's safe to
	 * call aio_complete is when we return -EIOCBQUEUED, so we key on that.
	 * This had *better* be the only place that raises -EIOCBQUEUED.
	 */
	BUG_ON(retval == -EIOCBQUEUED);

	/*
	 * For file extending writes updating i_size before data
	 * writeouts complete can expose uninitialized blocks. So
	 * even for AIO, we need to wait for i/o to complete before
	 * returning in this case.
	 */
	if (!is_sync_kiocb(iocb) &&
	    retval == 0 && dio->result &&
	    ((rw == READ) ||
	     (offset + size <= dio->i_size &&
	      dio->result == size))) {
		if (atomic_long_dec_and_test(&dio->refcount))
			retval = dio_complete(dio, offset, retval, false);
		else
			retval = -EIOCBQUEUED;
	} else {
		dio_wait_completion(dio);
		retval = dio_complete(dio, offset, retval, false);
		BUG_ON(retval == -EIOCBQUEUED);
	}

	return retval;
}

ssize_t
__blockdev_direct_IO(int rw, struct kiocb *iocb, struct inode *inode,
	struct block_device *bdev, const struct iovec *iov, loff_t offset,
	unsigned long nr_segs, get_block_t get_block, dio_iodone_t end_io,
	dio_submit_t submit_io,	int flags)
{
	/*
	 * The block device state is needed in the end to finally
	 * submit everything.  Since it's likely to be cache cold
	 * prefetch it here as first thing to hide some of the
	 * latency.
	 *
	 * Attempt to prefetch the pieces we likely need later.
	 */
	prefetch(&bdev->bd_disk->part_tbl);
	prefetch(bdev->bd_queue);
	prefetch((char *)bdev->bd_queue + SMP_CACHE_BYTES);

	return do_blockdev_direct_IO(rw, iocb, inode, bdev, iov, offset,
				     nr_segs, get_block, end_io,
				     submit_io, flags);
}

EXPORT_SYMBOL(__blockdev_direct_IO);

static __init int dio_init(void)
{
	dio_pool = bioset_create(4, offsetof(struct dio, bio));
	if (!dio_pool)
		return -ENOMEM;
	return 0;
}
module_init(dio_init)
