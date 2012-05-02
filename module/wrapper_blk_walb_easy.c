/**
 * wrapper_blk_walb_easy.c - WalB block device with Easy Algorithm for test.
 *
 * Copyright(C) 2012, Cybozu Labs, Inc.
 * @author HOSHINO Takashi <hoshino@labs.cybozu.co.jp>
 */
#include <linux/blkdev.h>
#include <linux/atomic.h>
#include <linux/list.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/spinlock.h>

#include "wrapper_blk.h"
#include "wrapper_blk_walb.h"

/*******************************************************************************
 * Static data definition.
 *******************************************************************************/

/**
 * Main queue to process requests.
 * This should be prepared per device.
 */
#define WQ_REQ_LIST_NAME "wq_req_list"
struct workqueue_struct *wq_req_list_ = NULL;

/**
 * Queue for flush requests.
 */
#define WQ_REQ_FLUSH_NAME "wq_req_flush"
struct workqueue_struct *wq_req_flush_ = NULL;

/**
 * Flush work.
 *
 * if flush_req is NULL, packs in the list can be executed in parallel,
 * else, run flush_req first, then enqueue packs in the list.
 */
struct flush_work
{
	struct work_struct work;
	struct list_head list; /* list entry */
	struct wrapper_blk_dev *wdev;
	struct request *flush_req; /* flush request if flush */
	int is_restart_queue; /* If non-zero, the task must restart queue. */

	struct list_head wpack_list; /* list head of writepack. */
	struct list_head rpack_list; /* list head of readpack. */
};
/* kmem_cache for flush_work. */
#define KMEM_CACHE_FLUSH_WORK_NAME "flush_work_cache"
struct kmem_cache *flush_work_cache_ = NULL;

/**
 * Request entry struct.
 */
struct req_entry
{
	struct list_head list; /* list entry */
	struct request *req;
	struct list_head bio_entry_list; /* list head of bio_entry */
	bool is_submitted; /* true after submitted. */
};
/* kmem cache for dbio. */
#define KMEM_CACHE_REQ_ENTRY_NAME "req_entry_cache"
struct kmem_cache *req_entry_cache_ = NULL;

/**
 * A pack.
 * There are no overlapping requests in a pack.
 */
struct pack
{
	struct list_head list; /* list entry. */
	struct list_head req_ent_list; /* list head of req_entry. */
	bool is_write; /* true if write, or read. */
	u64 lsid; /* lsid of the pack if write. */
        u16 n_sectors; /* IO size of total requests [logical block]. */
};
#define KMEM_CACHE_PACK_NAME "pack_cache"
struct kmem_cache *pack_cache_ = NULL;

/* bio as a list entry. */
struct bio_entry
{
	struct list_head list; /* list entry */
	struct bio *bio;
	struct completion done;
	unsigned int bi_size; /* keep bi_size at initialization,
				 because bio->bi_size will be 0 after endio. */
	int error; /* bio error status. */
};
/* kmem cache for dbio. */
#define KMEM_CACHE_BIO_ENTRY_NAME "bio_entry_cache"
struct kmem_cache *bio_entry_cache_ = NULL;

/*******************************************************************************
 * Macros definition.
 *******************************************************************************/

#define create_readpack(gfp_mask) create_pack(false, gfp_mask)
#define create_writepack(gfp_mask) create_pack(true, gfp_mask)

/*******************************************************************************
 * Static functions prototype.
 *******************************************************************************/

/* Print request flags for debug. */
static void print_req_flags(struct request *req);

/* flush_work related. */
static struct flush_work* create_flush_work(
	struct request *flush_req,
	struct wrapper_blk_dev *wdev, gfp_t gfp_mask);
static void destroy_flush_work(struct flush_work *work);

/* req_entry related. */
static struct req_entry* create_req_entry(struct request *req, gfp_t gfp_mask);
static void destroy_req_entry(struct req_entry *reqe);

/* bio_entry related. */
static void bio_entry_end_io(struct bio *bio, int error);
static struct bio_entry* create_bio_entry(
	struct bio *bio, struct block_device *bdev, gfp_t gfp_mask);
static void destroy_bio_entry(struct bio_entry *bioe);

/* pack related. */
static struct pack* create_pack(bool is_write, gfp_t gfp_mask);
static void destroy_pack(struct pack *pack);
static bool pack_add_reqe(struct pack *pack, struct req_entry *reqe);

/* helper function. */
static bool pack_add_req(struct list_head *pack_list,
			struct pack **packp, struct request *req);
static u64 calc_lsid(u64 next_lsid, struct list_head *wpack_list);


/* Request flush_work tasks. */
static void flush_work_task(struct work_struct *work); /* in parallel. */
static void req_flush_task(struct work_struct *work); /* sequential. */

/* Helper functions. */
static bool create_bio_entry_list(struct req_entry *reqe, struct wrapper_blk_dev *wdev);
static void submit_req_entry(struct req_entry *reqe);
static void wait_for_req_entry(struct req_entry *reqe);

/*******************************************************************************
 * Static functions definition.
 *******************************************************************************/

/**
 * Print request flags for debug.
 */
UNUSED
static void print_req_flags(struct request *req)
{
	LOGd("REQ_FLAGS: "
		"%s%s%s%s%s"
		"%s%s%s%s%s"
		"%s%s%s%s%s"
		"%s%s%s%s%s"
		"%s%s%s%s%s"
		"%s%s%s%s\n", 
		((req->cmd_flags & REQ_WRITE) ?              "REQ_WRITE" : ""),
		((req->cmd_flags & REQ_FAILFAST_DEV) ?       " REQ_FAILFAST_DEV" : ""),
		((req->cmd_flags & REQ_FAILFAST_TRANSPORT) ? " REQ_FAILFAST_TRANSPORT" : ""),
		((req->cmd_flags & REQ_FAILFAST_DRIVER) ?    " REQ_FAILFAST_DRIVER" : ""),
		((req->cmd_flags & REQ_SYNC) ?               " REQ_SYNC" : ""),
		((req->cmd_flags & REQ_META) ?               " REQ_META" : ""),
		((req->cmd_flags & REQ_PRIO) ?               " REQ_PRIO" : ""),
		((req->cmd_flags & REQ_DISCARD) ?            " REQ_DISCARD" : ""),
		((req->cmd_flags & REQ_NOIDLE) ?             " REQ_NOIDLE" : ""),
		((req->cmd_flags & REQ_RAHEAD) ?             " REQ_RAHEAD" : ""),
		((req->cmd_flags & REQ_THROTTLED) ?          " REQ_THROTTLED" : ""),
		((req->cmd_flags & REQ_SORTED) ?             " REQ_SORTED" : ""),
		((req->cmd_flags & REQ_SOFTBARRIER) ?        " REQ_SOFTBARRIER" : ""),
		((req->cmd_flags & REQ_FUA) ?                " REQ_FUA" : ""),
		((req->cmd_flags & REQ_NOMERGE) ?            " REQ_NOMERGE" : ""),
		((req->cmd_flags & REQ_STARTED) ?            " REQ_STARTED" : ""),
		((req->cmd_flags & REQ_DONTPREP) ?           " REQ_DONTPREP" : ""),
		((req->cmd_flags & REQ_QUEUED) ?             " REQ_QUEUED" : ""),
		((req->cmd_flags & REQ_ELVPRIV) ?            " REQ_ELVPRIV" : ""),
		((req->cmd_flags & REQ_FAILED) ?             " REQ_FAILED" : ""),
		((req->cmd_flags & REQ_QUIET) ?              " REQ_QUIET" : ""),
		((req->cmd_flags & REQ_PREEMPT) ?            " REQ_PREEMPT" : ""),
		((req->cmd_flags & REQ_ALLOCED) ?            " REQ_ALLOCED" : ""),
		((req->cmd_flags & REQ_COPY_USER) ?          " REQ_COPY_USER" : ""),
		((req->cmd_flags & REQ_FLUSH) ?              " REQ_FLUSH" : ""),
		((req->cmd_flags & REQ_FLUSH_SEQ) ?          " REQ_FLUSH_SEQ" : ""),
		((req->cmd_flags & REQ_IO_STAT) ?            " REQ_IO_STAT" : ""),
		((req->cmd_flags & REQ_MIXED_MERGE) ?        " REQ_MIXED_MERGE" : ""),
		((req->cmd_flags & REQ_SECURE) ?             " REQ_SECURE" : ""));
}

/**
 * Create a flush_work.
 *
 * RETURN:
 * NULL if failed.
 * CONTEXT:
 * Any.
 */
static struct flush_work* create_flush_work(
	struct request *flush_req,
	struct wrapper_blk_dev *wdev,
	gfp_t gfp_mask)
{
	struct flush_work *work;

	ASSERT(wdev);
	ASSERT(flush_work_cache_);

	work = kmem_cache_alloc(flush_work_cache_, gfp_mask);
	if (!work) {
		goto error0;
	}
	INIT_LIST_HEAD(&work->list);
	work->wdev = wdev;
	work->flush_req = flush_req;
	work->is_restart_queue = 0;
	INIT_LIST_HEAD(&work->wpack_list);
	INIT_LIST_HEAD(&work->rpack_list);
        
	return work;
error0:
	return NULL;
}

/**
 * Destory a flush_work.
 */
static void destroy_flush_work(struct flush_work *work)
{
	struct pack *pack, *next;

	if (!work) { return; }
	
	list_for_each_entry_safe(pack, next, &work->rpack_list, list) {
		list_del(&pack->list);
		destroy_pack(pack);
	}
	list_for_each_entry_safe(pack, next, &work->wpack_list, list) {
		list_del(&pack->list);
		destroy_pack(pack);
	}
#ifdef WALB_DEBUG
	work->flush_req = NULL;
	work->wdev = NULL;
	INIT_LIST_HEAD(&work->rpack_list);
	INIT_LIST_HEAD(&work->wpack_list);
#endif
	kmem_cache_free(flush_work_cache_, work);
}

/**
 * Create req_entry struct.
 */
static struct req_entry* create_req_entry(struct request *req, gfp_t gfp_mask)
{
	struct req_entry *reqe;

	reqe = kmem_cache_alloc(req_entry_cache_, gfp_mask);
	if (!reqe) {
		goto error0;
	}
	ASSERT(req);
	reqe->req = req;
	INIT_LIST_HEAD(&reqe->list);
	INIT_LIST_HEAD(&reqe->bio_entry_list);
	reqe->is_submitted = false;
        
	return reqe;
error0:
	return NULL;
}

/**
 * Destroy a req_entry.
 */
static void destroy_req_entry(struct req_entry *reqe)
{
	struct bio_entry *bioe, *next;

	if (reqe) {
		list_for_each_entry_safe(bioe, next, &reqe->bio_entry_list, list) {
			list_del(&bioe->list);
			destroy_bio_entry(bioe);
		}
#ifdef WALB_DEBUG
		reqe->req = NULL;
		INIT_LIST_HEAD(&reqe->list);
		INIT_LIST_HEAD(&reqe->bio_entry_list);
#endif
		kmem_cache_free(req_entry_cache_, reqe);
	}
}

/**
 * endio callback for bio_entry.
 */
static void bio_entry_end_io(struct bio *bio, int error)
{
	struct bio_entry *bioe = bio->bi_private;
	UNUSED int uptodate = test_bit(BIO_UPTODATE, &bio->bi_flags);
	ASSERT(bioe);
	ASSERT(bioe->bio == bio);
	ASSERT(uptodate);
        
	/* LOGd("bio_entry_end_io() begin.\n"); */
	bioe->error = error;
	bio_put(bio);
	bioe->bio = NULL;
	complete(&bioe->done);
	/* LOGd("bio_entry_end_io() end.\n"); */
}

/**
 * Create a bio_entry.
 *
 * @bio original bio.
 * @bdev block device to forward bio.
 */
static struct bio_entry* create_bio_entry(
	struct bio *bio, struct block_device *bdev, gfp_t gfp_mask)
{
	struct bio_entry *bioe;
	struct bio *biotmp;

	/* LOGd("create_bio_entry() begin.\n"); */

	bioe = kmem_cache_alloc(bio_entry_cache_, gfp_mask);
	if (!bioe) {
		LOGd("kmem_cache_alloc() failed.");
		goto error0;
	}
	init_completion(&bioe->done);
	bioe->error = 0;
	bioe->bi_size = bio->bi_size;

	/* clone bio */
	bioe->bio = NULL;
	biotmp = bio_clone(bio, gfp_mask);
	if (!biotmp) {
		LOGe("bio_clone() failed.");
		goto error1;
	}
	biotmp->bi_bdev = bdev;
	biotmp->bi_end_io = bio_entry_end_io;
	biotmp->bi_private = bioe;
	bioe->bio = biotmp;
        
	/* LOGd("create_bio_entry() end.\n"); */
	return bioe;

error1:
	destroy_bio_entry(bioe);
error0:
	LOGe("create_bio_entry() end with error.\n");
	return NULL;
}

/**
 * Destroy a bio_entry.
 */
static void destroy_bio_entry(struct bio_entry *bioe)
{
	/* LOGd("destroy_bio_entry() begin.\n"); */
        
	if (!bioe) {
		return;
	}

	if (bioe->bio) {
		LOGd("bio_put %p\n", bioe->bio);
		bio_put(bioe->bio);
		bioe->bio = NULL;
	}
	kmem_cache_free(bio_entry_cache_, bioe);

	/* LOGd("destroy_bio_entry() end.\n"); */
}

/**
 * Create a pack.
 */
static struct pack* create_pack(bool is_write, gfp_t gfp_mask)
{
	struct pack *pack;

	/* LOGd("create_bio_entry() begin.\n"); */

	pack = kmem_cache_alloc(pack_cache_, gfp_mask);
	if (!pack) {
		LOGd("kmem_cache_alloc() failed.");
		goto error0;
	}
	LIST_HEAD_INIT(&pack->list);
	LIST_HEAD_INIT(&pack->req_ent_list);
	pack->is_write = is_write;
	
	return pack;
#if 0
error1:
	destory_pack(pack);
#endif
error0:
	LOGe("create_pack() end with error.\n");
	return NULL;
}

/**
 * Destory a pack.
 */
static void destroy_pack(struct pack *pack)
{
	struct req_entry *reqe, *next;
	
	if (!pack) { return; }
	
	list_for_each_entry_safe(reqe, next, &pack->req_ent_list, list) {
		list_del(&reqe->list);
		destroy_req_entry(reqe);
	}
#ifdef WALB_DEBUG
	INIT_LIST_HEAD(&work->req_ent_list);
#endif
	kmem_cache_free(pack_cache_, pack);
}

/**
 * Add a request entry to a pack.
 *
 * @pack pack to added.
 * @reqe req_entry to add.
 * @max_sectors_in_pack maximum number of sectors in a pack.
 *
 * If an overlapping request exists or
 *   total number of sectors in the pack exceeds @max_pack_sectors,
 *   add nothing and return false.
 * 
 * Else, add the request and return true.
 */
static bool pack_add_reqe(struct pack *pack, struct req_entry *reqe,
			unsigned int max_sectors_in_pack)
{
	struct req_entry *tmp_reqe;
	
	ASSERT(pack);
	ASSERT(reqe);
	ASSERT(pack->is_write == (reqe->req->cmd_flags & REQ_WRITE != 0));

	/* Check pack size limitation. */
	if (pack->sectors + blk_rq_sectors(blk_reqe->req) > max_pack_sectors) {
		return false;
	}
	
	/* Search overlapping requests. */
	list_for_each_entry(tmp_reqe, &pack->req_ent_list, list) {
		if (is_overlap_req(tmp_reqe->req, reqe->req)) {
			return false;
		}
	}

	list_add_tail(reqe, &pack->req_ent_list);
	return true;
}

/**
 * Helper function to add request to a pack.
 * 
 * @pack_list pack list.
 * @packp pointer to pack pointer.
 * @req reqeuest to add.
 * @max_sectors_in_pack maximum number of sectors in a pack.
 *
 * RETURN:
 *   true: succeeded.
 *   false: out of memory.
 *
 * CONTEXT:
 *   IRQ: no, ATOMIC: yes.
 *   queue lock is held.
 */
static bool pack_add_req(
	struct list_head *pack_list, struct pack **packp, struct request *req
	unsigned int max_sectors_in_pack)
{
	struct req_entry *reqe;
	struct pack *pack;
	bool ret;
	bool is_write;

	ASSERT(pack_list);
	ASSERT(packp);
	ASSERT(req);

	pack = *packp;

	is_write = (req->cmd_flags & REQ_WRITE) != 0;
	ASSERT(pack->is_write == is_write);
	
	reqe = create_req_entry(req, GFP_ATOMIC);
	if (!reqe) {
		goto error0;
	}
	if (!pack_add_reqe(pack, reqe, max_sectors_in_pack)) {
		/* overlap found then create a new pack. */
		list_add_tail(&pack->list, pack_list);
		pack = create_pack(is_write, GFP_ATOMIC);
		if (!pack) { goto error1; }
		ret = pack_add_reqe(pack, reqe);
		ASSERT(ret);
		*packp = pack;
	}
	return true;
error1:
	destroy_req_entry(reqe);
error0:
	return false;
}

/**
 * Calculate each writepack lsid.
 *
 * @pbs physical block size in bytes.
 *
 * Logical block size is fixed 512 bytes.
 *
 * RETURN:
 *   next lsid.
 *
 * CONTEXT:
 *   any.
 */
static u64 calc_lsid(u64 next_lsid, struct list_head *wpack_list, 
		unsigend int pbs)
{
	struct pack *wpack;
	struct req_entry *reqe;
	sector_t n_sectors;
	
	list_for_each_entry(wpack, wpack_list, list) {

		ASSERT(wpack->is_write);
		n_sectors = 0;
		list_for_each_entry(reqe, &wpack->req_ent_list, list) {

			ASSERT(reqe);
			ASSERT(reqe->req);
			n_sectors += blk_rq_sectors(reqe->req);
		}
		
	}

	


}

/**
 * Create bio_entry list for a request.
 *
 * RETURN:
 *     true if succeed, or false.
 * CONTEXT:
 *     Non-IRQ. Non-atomic.
 */
static bool create_bio_entry_list(struct req_entry *reqe, struct wrapper_blk_dev *wdev)

{
	struct bio_entry *bioe, *next;
	struct bio *bio;
	struct pdata *pdata = wdev->private_data;
	struct block_device *bdev = pdata->ddev;
        
	ASSERT(reqe);
	ASSERT(reqe->req);
	ASSERT(wdev);
	ASSERT(list_empty(&reqe->bio_entry_list));
        
	/* clone all bios. */
	__rq_for_each_bio(bio, reqe->req) {
		/* clone bio */
		bioe = create_bio_entry(bio, bdev, GFP_NOIO);
		if (!bioe) {
			LOGd("create_bio_entry() failed.\n"); 
			goto error1;
		}
		list_add_tail(&bioe->list, &reqe->bio_entry_list);
	}

	return true;
error1:
	list_for_each_entry_safe(bioe, next, &reqe->bio_entry_list, list) {
		list_del(&bioe->list);
		destroy_bio_entry(bioe);
	}
	ASSERT(list_empty(&reqe->bio_entry_list));
	return false;
}

/**
 * Submit all bios in a bio_entry.
 *
 * @reqe target req_entry.
 */
static void submit_req_entry(struct req_entry *reqe)
{
	struct bio_entry *bioe;
	list_for_each_entry(bioe, &reqe->bio_entry_list, list) {
		generic_make_request(bioe->bio);
	}
	reqe->is_submitted = true;
}

/**
 * Wait for completion and end request.
 *
 * @reqe target req_entry.
 */
static void wait_for_req_entry(struct req_entry *reqe)
{
	struct bio_entry *bioe, *next;
	int remaining;

	ASSERT(reqe);
        
	remaining = blk_rq_bytes(reqe->req);
	list_for_each_entry_safe(bioe, next, &reqe->bio_entry_list, list) {
		wait_for_completion(&bioe->done);
		blk_end_request(reqe->req, bioe->error, bioe->bi_size);
		remaining -= bioe->bi_size;
		list_del(&bioe->list);
		destroy_bio_entry(bioe);
	}
	ASSERT(remaining == 0);
}

/**
 * Execute request list.
 *
 * (1) Clone all bios related each request in the list.
 * (2) Submit them.
 * (3) wait completion of all bios.
 * (4) notify completion to the block layer.
 * (5) free memories.
 *
 * CONTEXT:
 *   Non-IRQ. Non-atomic.
 *   Request queue lock is not held.
 *   Other tasks may be running concurrently.
 */
static void flush_work_task(struct work_struct *work)
{
	struct flush_work *rlwork = container_of(work, struct flush_work, work);
	struct wrapper_blk_dev *wdev = rlwork->wdev;
	struct req_entry *reqe, *next;
	struct blk_plug plug;

	/* LOGd("flush_work_task begin.\n"); */
        
	ASSERT(rlwork->flush_req == NULL);

	/* prepare and submit */
	blk_start_plug(&plug);
	list_for_each_entry(reqe, &rlwork->req_entry_list, list) {
		if (!create_bio_entry_list(reqe, wdev)) {
			LOGe("create_bio_entry_list failed.\n");
			goto error0;
		}
		submit_req_entry(reqe);
	}
	blk_finish_plug(&plug);

	/* wait completion and end requests. */
	list_for_each_entry_safe(reqe, next, &rlwork->req_entry_list, list) {
		wait_for_req_entry(reqe);
		list_del(&reqe->list);
		destroy_req_entry(reqe);
	}
	/* destroy work struct */
	destroy_flush_work(rlwork);
	/* LOGd("flush_work_task end.\n"); */
	return;

error0:
	list_for_each_entry_safe(reqe, next, &rlwork->req_entry_list, list) {
		if (reqe->is_submitted) {
			wait_for_req_entry(reqe);
		} else {
			blk_end_request_all(reqe->req, -EIO);
		}
		list_del(&reqe->list);
		destroy_req_entry(reqe);
	}
	destroy_flush_work(rlwork);
	LOGd("flush_work_task error.\n");
}

/**
 * Request flush task.
 */
static void req_flush_task(struct work_struct *work)
{
	struct flush_work *rlwork = container_of(work, struct flush_work, work);
	struct request_queue *q = rlwork->wdev->queue;
	int is_restart_queue = rlwork->is_restart_queue;
	unsigned long flags;
        
	LOGd("req_flush_task begin.\n");
	ASSERT(rlwork->flush_req);

	/* Flush previous all requests. */
	flush_workqueue(wq_req_list_);
	blk_end_request_all(rlwork->flush_req, 0);

	/* Restart queue if required. */
	if (is_restart_queue) {
		spin_lock_irqsave(q->queue_lock, flags);
		ASSERT(blk_queue_stopped(q));
		blk_start_queue(q);
		spin_unlock_irqrestore(q->queue_lock, flags);
	}
        
	if (list_empty(&rlwork->req_entry_list)) {
		destroy_flush_work(rlwork);
	} else {
		/* Enqueue the following requests */
		rlwork->flush_req = NULL;
		INIT_WORK(&rlwork->work, flush_work_task);
		queue_work(wq_req_list_, &rlwork->work);
	}
	LOGd("req_flush_task end.\n");
}


/**
 * Enqueue all works in a list.
 *
 * CONTEXT:
 *     in_interrupt(): false. is_atomic(): true.
 *     queue lock is held.
 */
static void enqueue_work_list(struct list_head *listh, struct request_queue *q)
{
	struct flush_work *work;
	
	list_for_each_entry(work, listh, list) {
		if (work->flush_req) {
			if (list_is_last(&work->list, listh)) {
				work->is_restart_queue = true;
				blk_stop_queue(q);
			}
			INIT_WORK(&work->work, req_flush_task);
			queue_work(wq_req_flush_, &work->work);
		} else {
			INIT_WORK(&work->work, flush_work_task);
			queue_work(wq_req_list_, &work->work);
		}
	}
}

/*******************************************************************************
 * Global functions definition.
 *******************************************************************************/

/**
 * Make requrest callback.
 *
 * CONTEXT:
 *     IRQ: no. ATOMIC: yes.
 *     queue lock is held.
 */
void wrapper_blk_req_request_fn(struct request_queue *q)
{
	struct wrapper_blk_dev *wdev = wdev_get_from_queue(q);
	struct pdata *pdata = pdata_get_from_wdev(wdev);
	struct request *req;
	struct req_entry *reqe;
	struct flush_work *fwork;
	struct list_head listh;
	struct pack *wpack, *rpack, *pack;
	struct bool ret;
	struct bool is_write;
	u64 next_lsid;
	unsigned int max_sectors_in_pack
		= wdev->blksiz.n_lb_in_pb * 65535; /* 16bit unsigned */
	
	bool errorOccurd = false;

	next_lsid = pdata->next_lsid;

	INIT_LIST_HEAD(&listh);
	fwork = create_flush_work(NULL, wdev, GFP_ATOMIC);
	if (!fwork) { goto error0; }
	wpack = create_writepack(GFP_ATOMIC);
	if (!wpack) { goto error1; }
	rpack = create_readpack(GFP_ATOMIC);
	if (!rpack) { goto error2; }

	while ((req = blk_fetch_request(q)) != NULL) {

		/* print_req_flags(req); */
		if (errorOccurd) { goto req_error; }

		if (req->cmd_flags & REQ_FLUSH) {
			LOGd("REQ_FLUSH request with size %u.\n", blk_rq_bytes(req));
			list_add_tail(&fwork->list, &listh);
			fwork = create_flush_work(req, wdev, GFP_ATOMIC);
			if (!fwork) {
				errorOccurd = true;
				goto req_error;
			}
		} else if (req->cmd_flags & REQ_WRITE) {
			ret = pack_add_req(fwork->wpack_list, &wpack, req,
					max_sectors_in_pack);
			if (!ret) { goto req_error; }
		} else {
			ret = pack_add_req(fwork->rpack_list, &rpack, req,
				max_sectors_in_pack);
			if (!ret) { goto req_error; }
		}
		continue;
	req_error:
		__blk_end_request_all(req, -EIO);
	}
	list_add_tail(&wpack->list, &fwork->wpack_list);
	list_add_tail(&rpack->list, &fwork->rpack_list);
	list_add_tail(&fwork->list, &listh);

	list_for_each_entry(fwork, &listh, list) {

		next_lsid = calc_lsid(next_lsid, &fwork->wpack_list);
	}
	
	/* now editing */
		
	
	enqueue_fwork_list(&listh, q);
	INIT_LIST_HEAD(&listh);
	/* LOGd("wrapper_blk_req_request_fn: end.\n"); */
	return;
#if 0
error3:
	destroy_pack(rpack);
#endif
error2:
	destroy_pack(wpack);	
error1:
	destroy_flush_work(fwork);
error0:
	while ((req = blk_fetch_request(q)) != NULL) {
		__blk_end_request_all(req, -EIO);
	}
	/* LOGe("wrapper_blk_req_request_fn: error.\n"); */
}

/**
 * Deprecated on 20120501.
 * Will be removed.
 */
DEPRECATED
void wrapper_blk_req_request_fn_old(struct request_queue *q)
{
	struct wrapper_blk_dev *wdev = wdev_get_from_queue(q);
	struct request *req;
	struct req_entry *reqe;
	struct flush_work *work;
	struct list_head listh;
	bool errorOccurd = false;

	/* LOGd("wrapper_blk_req_request_fn: in_interrupt: %lu in_atomic: %d\n", */
	/* 	in_interrupt(), in_atomic()); */

	INIT_LIST_HEAD(&listh);
	work = create_flush_work(NULL, wdev, GFP_ATOMIC);
	if (!work) { goto error0; }
	
	while ((req = blk_fetch_request(q)) != NULL) {

		/* print_req_flags(req); */

		if (errorOccurd) {
			__blk_end_request_all(req, -EIO);
			continue;
		}

		if (req->cmd_flags & REQ_FLUSH) {
			LOGd("REQ_FLUSH request with size %u.\n", blk_rq_bytes(req));

			list_add_tail(&work->list, &listh);
			work = create_flush_work(req, wdev, GFP_ATOMIC);
			if (!work) {
				errorOccurd = true;
				__blk_end_request_all(req, -EIO);
				continue;
			}
		} else {
			reqe = create_req_entry(req, GFP_ATOMIC);
			if (!reqe) {
				__blk_end_request_all(req, -EIO);
				continue;
			}
			list_add_tail(&reqe->list, &work->req_entry_list);
		}
	}
	list_add_tail(&work->list, &listh);
	enqueue_work_list(&listh, q);
	INIT_LIST_HEAD(&listh);
	/* LOGd("wrapper_blk_req_request_fn: end.\n"); */
	return;
error0:
	while ((req = blk_fetch_request(q)) != NULL) {
		__blk_end_request_all(req, -EIO);
	}
	/* LOGe("wrapper_blk_req_request_fn: error.\n"); */
}
        
/* Called before register. */
bool pre_register(void)
{
	LOGd("pre_register called.");

	/* Prepare kmem_cache data. */
	flush_work_cache_ = kmem_cache_create(
		KMEM_CACHE_FLUSH_WORK_NAME,
		sizeof(struct flush_work), 0, 0, NULL);
	if (!flush_work_cache_) {
		LOGe("failed to create a kmem_cache.\n");
		goto error0;
	}
	req_entry_cache_ = kmem_cache_create(
		KMEM_CACHE_REQ_ENTRY_NAME,
		sizeof(struct req_entry), 0, 0, NULL);
	if (!req_entry_cache_) {
		LOGe("failed to create a kmem_cache.\n");
		goto error1;
	}
	bio_entry_cache_ = kmem_cache_create(
		KMEM_CACHE_BIO_ENTRY_NAME,
		sizeof(struct bio_entry), 0, 0, NULL);
	if (!bio_entry_cache_) {
		LOGe("failed to create a kmem_cache.\n");
		goto error2;
	}
	pack_cache_ = kmem_cache_create(
		KMEM_CACHE_PACK_NAME,
		sizeof(struct pack), 0, 0, NULL);
	if (pack_cache_) {
		LOGe("failed to create a kmem_cache.\n");
		goto error3;
	}
	
	/* prepare workqueue data. */
	wq_req_list_ = alloc_workqueue(WQ_REQ_LIST_NAME, WQ_MEM_RECLAIM, 0);
	if (!wq_req_list_) {
		LOGe("failed to allocate a workqueue.");
		goto error4;
	}
	wq_req_flush_ = create_singlethread_workqueue(WQ_REQ_FLUSH_NAME);
	if (!wq_req_flush_) {
		LOGe("failed to allocate a workqueue.");
		goto error5;
	}

	return true;

#if 0
error6:
	destroy_workqueue(wq_req_flush_);
#endif
error5:
	destroy_workqueue(wq_req_list_);
error4:	
	kmem_cache_destroy(pack_cache_);
error3:
	kmem_cache_destroy(bio_entry_cache_);
error2:
	kmem_cache_destroy(req_entry_cache_);
error1:
	kmem_cache_destroy(flush_work_cache_);
error0:
	return false;
}

/* Called after unregister. */
void post_unregister(void)
{
	LOGd("post_unregister called.");

	/* finalize workqueue data. */
	destroy_workqueue(wq_req_flush_);
	wq_req_flush_ = NULL;
	destroy_workqueue(wq_req_list_);
	wq_req_list_ = NULL;

	/* Destory kmem_cache data. */
	kmem_cache_destroy(pack_cache_);
	pack_cache_ = NULL;
	kmem_cache_destroy(bio_entry_cache_);
	bio_entry_cache_ = NULL;
	kmem_cache_destroy(req_entry_cache_);
	req_entry_cache_ = NULL;
	kmem_cache_destroy(flush_work_cache_);
	flush_work_cache_ = NULL;
}

/* end of file. */
