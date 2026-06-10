/*
 * Trabalho 3 -- SSTF (Shortest Seek Time First) disk scheduler
 *
 * Implemented on top of the blk-mq (elevator) infrastructure. Uses
 * block/mq-deadline.c from kernel 6.12.27 as reference.
 *
 * Behavior:
 *   - Requests are queued internally (insert_requests).
 *   - Dispatch starts only when the queue reaches "queue_size" requests OR
 *     when "max_wait_ms" elapses after the last request (hrtimer).
 *   - On each dispatch, the request whose sector is closest to the current
 *     head position is chosen (SSTF policy).
 *
 * Parameters (load time):
 *   queue_size  - number of requests before dispatch (20-100)
 *   max_wait_ms - maximum wait time in ms (20-100)
 *   debug       - debug messages in the kernel log
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>

#include "elevator.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Guilherme Hoffmann, Endrew Soares, João Sbardelotto, George Rother");
MODULE_DESCRIPTION("SSTF (Shortest Seek Time First) disk scheduler");
MODULE_VERSION("1.0.0");

/* ---- Load-time configurable parameters ---- */
static int queue_size = 50;
module_param(queue_size, int, 0444);
MODULE_PARM_DESC(queue_size, "Number of requests queued before dispatch (20-100)");

static int max_wait_ms = 50;
module_param(max_wait_ms, int, 0444);
MODULE_PARM_DESC(max_wait_ms, "Maximum wait time in ms before dispatching (20-100)");

static bool debug;
module_param(debug, bool, 0644);
MODULE_PARM_DESC(debug, "Debug mode: messages in the kernel log");

#define sstf_dbg(fmt, ...) \
	do { if (debug) pr_info("sstf: " fmt, ##__VA_ARGS__); } while (0)

/*
 * ---- SSTF data structure ----
 * Per-request-queue state. Lives in elevator_queue->elevator_data and is
 * allocated in init_sched / freed in exit_sched. All fields below are
 * protected by 'lock'.
 */
struct sstf_data {
	struct list_head      queue;      /* pending requests (linked via rq->queuelist) */
	unsigned int          count;      /* number of requests currently in the queue */
	sector_t              head_pos;   /* current head position (last served sector) */
	bool                  ready;      /* latch: queue full OR timer expired -> allow dispatch */
	struct hrtimer        timer;      /* fires max_wait_ms after the last arrival */
	spinlock_t            lock;       /* serializes all access to this struct */
	struct blk_mq_hw_ctx *hctx;       /* context for blk_mq_run_hw_queue() from the timer */

	/* statistics for the comparison (debug mode) */
	sector_t              fcfs_head;  /* simulated head serving in arrival order */
	unsigned long long    fcfs_total; /* sectors traveled WITHOUT reordering (FCFS) */
	unsigned long long    sstf_total; /* sectors traveled WITH reordering (SSTF) */
};

/* absolute distance between two sectors (seek cost between 'a' and 'b') */
static inline sector_t sstf_dist(sector_t a, sector_t b)
{
	return (a > b) ? (a - b) : (b - a);
}

/*
 * ---- Timer: forces the dispatch of a non-full queue after max_wait_ms ----
 * Prevents requests from getting stuck indefinitely when the queue never
 * reaches queue_size. On expiry, it raises the 'ready' latch and kicks the hw
 * queue so blk-mq calls dispatch_request. Runs in softirq context, hence the
 * use of spin_lock_irqsave.
 */
static enum hrtimer_restart sstf_timeout(struct hrtimer *t)
{
	struct sstf_data *sd = container_of(t, struct sstf_data, timer);
	unsigned long flags;
	bool run = false;

	spin_lock_irqsave(&sd->lock, flags);
	/* only act if there are still requests and dispatch was not yet released */
	if (sd->count > 0 && !sd->ready) {
		sd->ready = true;
		run = true;
		sstf_dbg("timeout: dispatching non-full queue (%u req)\n", sd->count);
	}
	spin_unlock_irqrestore(&sd->lock, flags);

	/* kick the queue outside the lock to avoid calling blk-mq holding the spinlock */
	if (run && sd->hctx)
		blk_mq_run_hw_queue(sd->hctx, true);

	return HRTIMER_NORESTART;	/* one-shot timer; rearmed in insert_requests */
}

/*
 * ---- Scheduler life cycle ----
 * Called when the scheduler is activated for queue 'q' (e.g. echo sstf >
 * /sys/block/<dev>/queue/scheduler). Allocates and initializes sstf_data and
 * connects it to the queue via q->elevator.
 */
static int sstf_init_sched(struct request_queue *q, struct elevator_type *e)
{
	struct sstf_data *sd;
	struct elevator_queue *eq;

	/* scheduler container required by the elevator infrastructure */
	eq = elevator_alloc(q, e);
	if (!eq)
		return -ENOMEM;

	/* kzalloc: zeroes everything; on the queue's NUMA node for memory locality */
	sd = kzalloc_node(sizeof(*sd), GFP_KERNEL, q->node);
	if (!sd) {
		kobject_put(&eq->kobj);	/* undo elevator_alloc on failure */
		return -ENOMEM;
	}

	INIT_LIST_HEAD(&sd->queue);
	sd->count = 0;
	sd->head_pos = 0;
	sd->ready = false;
	sd->hctx = NULL;
	sd->fcfs_head = 0;
	sd->fcfs_total = 0;
	sd->sstf_total = 0;
	spin_lock_init(&sd->lock);

	/* monotonic timer in relative mode; callback set manually */
	hrtimer_init(&sd->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	sd->timer.function = sstf_timeout;

	eq->elevator_data = sd;

	/* SSTF reorders globally: force blk-mq to treat the queue as single-queue */
	blk_queue_flag_set(QUEUE_FLAG_SQ_SCHED, q);
	q->elevator = eq;

	sstf_dbg("init_sched: queue_size=%d max_wait_ms=%d\n", queue_size, max_wait_ms);
	return 0;
}

/*
 * Scheduler teardown: cancels the timer, prints the FCFS vs SSTF comparison
 * (if debug) and frees the state.
 */
static void sstf_exit_sched(struct elevator_queue *e)
{
	struct sstf_data *sd = e->elevator_data;

	hrtimer_cancel(&sd->timer);	/* make sure the callback does not run after kfree */

	if (debug) {
		unsigned long long reduction =
			(sd->fcfs_total >= sd->sstf_total) ?
			(sd->fcfs_total - sd->sstf_total) : 0ULL;

		pr_info("sstf: ===== final comparison (sectors traveled) =====\n");
		pr_info("sstf:   WITHOUT reordering (FCFS): %llu\n", sd->fcfs_total);
		pr_info("sstf:   WITH reordering (SSTF): %llu\n", sd->sstf_total);
		pr_info("sstf:   reduction: %llu sectors\n", reduction);
	}

	kfree(sd);
}

/*
 * ---- Inserting requests into the internal queue ----
 * blk-mq hands a batch of requests here ('list'). We move each one into the
 * internal queue (sd->queue), update the counter and the FCFS statistic, and
 * decide whether it is time to release the dispatch (latch 'ready').
 */
static void sstf_insert_requests(struct blk_mq_hw_ctx *hctx,
				 struct list_head *list, blk_insert_t flags)
{
	struct request_queue *q = hctx->queue;
	struct sstf_data *sd = q->elevator->elevator_data;
	unsigned long irqflags;

	spin_lock_irqsave(&sd->lock, irqflags);
	sd->hctx = hctx;	/* remember the hctx so the timer can kick the queue */

	while (!list_empty(list)) {
		struct request *rq = list_first_entry(list, struct request, queuelist);
		sector_t pos = blk_rq_pos(rq);	/* starting sector of the request */

		/* move rq from the blk-mq list to the tail of our queue */
		list_del_init(&rq->queuelist);
		list_add_tail(&rq->queuelist, &sd->queue);
		sd->count++;

		/*
		 * Account for the cost if we served in arrival order (FCFS):
		 * add the seek from the simulated head and advance it to 'pos'.
		 * Used only as a baseline to compare against the SSTF gain.
		 */
		sd->fcfs_total += sstf_dist(pos, sd->fcfs_head);
		sd->fcfs_head = pos;

		sstf_dbg("arrival: sector %llu (queue=%u)\n",
			 (unsigned long long)pos, sd->count);

		/* reached queue_size -> release the batch dispatch */
		if (sd->count >= (unsigned int)queue_size && !sd->ready) {
			sd->ready = true;
			sstf_dbg("queue full (%u): dispatching\n", sd->count);
		}
	}

	/* while not dispatching, (re)arm the timer from the last request */
	if (!sd->ready && sd->count > 0)
		hrtimer_start(&sd->timer, ms_to_ktime(max_wait_ms), HRTIMER_MODE_REL);

	spin_unlock_irqrestore(&sd->lock, irqflags);
}

/*
 * ---- Dispatch: pick the request closest to the head ----
 * Heart of the SSTF policy: scan the queue and return the request whose sector
 * is at the shortest distance from sd->head_pos. Only dispatches if the 'ready'
 * latch is raised (queue full or timeout). Returns NULL when there is nothing
 * to dispatch.
 */
static struct request *sstf_dispatch_request(struct blk_mq_hw_ctx *hctx)
{
	struct sstf_data *sd = hctx->queue->elevator->elevator_data;
	struct request *best = NULL, *rq;	/* best = closest request so far */
	unsigned long flags;
	sector_t best_dist = 0, pos, old_head;
	unsigned int remaining;
	char dir;				/* 'R'=forward, 'L'=backward (debug only) */

	spin_lock_irqsave(&sd->lock, flags);

	/* dispatch not yet released or empty queue: nothing to do */
	if (!sd->ready || sd->count == 0) {
		spin_unlock_irqrestore(&sd->lock, flags);
		return NULL;
	}

	/* linear search for the shortest seek from the current head position */
	list_for_each_entry(rq, &sd->queue, queuelist) {
		sector_t d = sstf_dist(blk_rq_pos(rq), sd->head_pos);

		if (!best || d < best_dist) {
			best = rq;
			best_dist = d;
		}
	}

	/* remove the chosen one from the internal queue */
	list_del_init(&best->queuelist);
	sd->count--;

	pos = blk_rq_pos(best);
	old_head = sd->head_pos;
	dir = (pos >= old_head) ? 'R' : 'L';
	sd->sstf_total += best_dist;	/* accumulate the real SSTF cost */
	sd->head_pos = pos;		/* move the head to the served sector */
	remaining = sd->count;

	if (sd->count == 0)
		sd->ready = false;	/* end of the batch: lower the latch and accumulate again */

	spin_unlock_irqrestore(&sd->lock, flags);

	sstf_dbg("served: sector %llu dir=%c dist=%llu (remaining=%u)\n",
		 (unsigned long long)pos, dir,
		 (unsigned long long)best_dist, remaining);

	return best;
}

/* has_work: tells blk-mq whether there is a pending dispatch -- true only with the 'ready' latch */
static bool sstf_has_work(struct blk_mq_hw_ctx *hctx)
{
	struct sstf_data *sd = hctx->queue->elevator->elevator_data;
	unsigned long flags;
	bool work;

	spin_lock_irqsave(&sd->lock, flags);
	work = sd->ready && sd->count > 0;
	spin_unlock_irqrestore(&sd->lock, flags);

	return work;
}

/* finish_request: mandatory elevator hook; SSTF keeps no per-rq state */
static void sstf_finish_request(struct request *rq)
{
}

/* operations table that registers SSTF with the elevator infrastructure */
static struct elevator_type sstf = {
	.ops = {
		.init_sched       = sstf_init_sched,
		.exit_sched       = sstf_exit_sched,
		.insert_requests  = sstf_insert_requests,
		.dispatch_request = sstf_dispatch_request,
		.has_work         = sstf_has_work,
		.finish_request   = sstf_finish_request,
	},
	.elevator_name = "sstf",
	.elevator_owner = THIS_MODULE,
};

/* module load: registers the scheduler, making "sstf" selectable in sysfs */
static int __init sstf_init(void)
{
	pr_info("sstf: registering (queue_size=%d, max_wait_ms=%d, debug=%d)\n",
		queue_size, max_wait_ms, debug);
	return elv_register(&sstf);
}

/* module unload: removes the scheduler from the kernel */
static void __exit sstf_exit(void)
{
	elv_unregister(&sstf);
	pr_info("sstf: unregistered\n");
}

module_init(sstf_init);
module_exit(sstf_exit);
