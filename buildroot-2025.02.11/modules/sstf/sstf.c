/*
 * Trabalho 3 -- Escalonador de disco SSTF (Shortest Seek Time First)
 *
 * Implementado sobre a infraestrutura blk-mq (elevator). Tem como referência o
 * block/mq-deadline.c do kernel 6.12.27.
 *
 * Comportamento:
 *   - As requisições são enfileiradas internamente (insert_requests).
 *   - O despacho só começa quando a fila atinge "queue_size" requisições OU
 *     quando o "max_wait_ms" expira após a última requisição (hrtimer).
 *   - Em cada despacho, escolhe-se a requisição cujo setor está mais próximo da
 *     posição atual do cabeçote (política SSTF).
 *
 * Parâmetros (tempo de carga):
 *   queue_size  - nº de requisições antes do despacho (20-100)
 *   max_wait_ms - tempo máximo de espera em ms (20-100)
 *   debug       - mensagens de depuração no log do kernel
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
MODULE_AUTHOR("Estudante");
MODULE_DESCRIPTION("Escalonador de disco SSTF (Shortest Seek Time First)");
MODULE_VERSION("1.0.0");

/* ---- Parâmetros configuráveis em tempo de carga ---- */
static int queue_size = 50;
module_param(queue_size, int, 0444);
MODULE_PARM_DESC(queue_size, "Numero de requisicoes enfileiradas antes do despacho (20-100)");

static int max_wait_ms = 50;
module_param(max_wait_ms, int, 0444);
MODULE_PARM_DESC(max_wait_ms, "Tempo maximo de espera em ms antes de despachar (20-100)");

static bool debug;
module_param(debug, bool, 0644);
MODULE_PARM_DESC(debug, "Modo de depuracao: mensagens no log do kernel");

#define sstf_dbg(fmt, ...) \
	do { if (debug) pr_info("sstf: " fmt, ##__VA_ARGS__); } while (0)

/* ---- Estrutura de dados do SSTF ---- */
struct sstf_data {
	struct list_head      queue;      /* requisições pendentes (via queuelist) */
	unsigned int          count;      /* nº de requisições na fila */
	sector_t              head_pos;   /* posição atual do cabeçote */
	bool                  ready;      /* latch: fila cheia ou timer expirou */
	struct hrtimer        timer;
	spinlock_t            lock;
	struct blk_mq_hw_ctx *hctx;       /* p/ blk_mq_run_hw_queue a partir do timer */

	/* estatísticas para a comparação (modo debug) */
	sector_t              fcfs_head;  /* cabeçote simulado servindo por ordem de chegada */
	unsigned long long    fcfs_total; /* setores percorridos SEM reordenação (FCFS) */
	unsigned long long    sstf_total; /* setores percorridos COM reordenação (SSTF) */
};

/* distância absoluta entre dois setores */
static inline sector_t sstf_dist(sector_t a, sector_t b)
{
	return (a > b) ? (a - b) : (b - a);
}

/* ---- Timer: força o despacho de uma fila não cheia após max_wait_ms ---- */
static enum hrtimer_restart sstf_timeout(struct hrtimer *t)
{
	struct sstf_data *sd = container_of(t, struct sstf_data, timer);
	unsigned long flags;
	bool run = false;

	spin_lock_irqsave(&sd->lock, flags);
	if (sd->count > 0 && !sd->ready) {
		sd->ready = true;
		run = true;
		sstf_dbg("timeout: despachando fila nao cheia (%u req)\n", sd->count);
	}
	spin_unlock_irqrestore(&sd->lock, flags);

	if (run && sd->hctx)
		blk_mq_run_hw_queue(sd->hctx, true);

	return HRTIMER_NORESTART;
}

/* ---- Ciclo de vida do escalonador ---- */
static int sstf_init_sched(struct request_queue *q, struct elevator_type *e)
{
	struct sstf_data *sd;
	struct elevator_queue *eq;

	eq = elevator_alloc(q, e);
	if (!eq)
		return -ENOMEM;

	sd = kzalloc_node(sizeof(*sd), GFP_KERNEL, q->node);
	if (!sd) {
		kobject_put(&eq->kobj);
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

	hrtimer_init(&sd->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	sd->timer.function = sstf_timeout;

	eq->elevator_data = sd;

	/* escalonador trabalha como fila única */
	blk_queue_flag_set(QUEUE_FLAG_SQ_SCHED, q);
	q->elevator = eq;

	sstf_dbg("init_sched: queue_size=%d max_wait_ms=%d\n", queue_size, max_wait_ms);
	return 0;
}

static void sstf_exit_sched(struct elevator_queue *e)
{
	struct sstf_data *sd = e->elevator_data;

	hrtimer_cancel(&sd->timer);

	if (debug) {
		unsigned long long reducao =
			(sd->fcfs_total >= sd->sstf_total) ?
			(sd->fcfs_total - sd->sstf_total) : 0ULL;

		pr_info("sstf: ===== comparacao final (setores percorridos) =====\n");
		pr_info("sstf:   SEM reordenacao (FCFS): %llu\n", sd->fcfs_total);
		pr_info("sstf:   COM reordenacao (SSTF): %llu\n", sd->sstf_total);
		pr_info("sstf:   reducao: %llu setores\n", reducao);
	}

	kfree(sd);
}

/* ---- Inserção de requisições na fila interna ---- */
static void sstf_insert_requests(struct blk_mq_hw_ctx *hctx,
				 struct list_head *list, blk_insert_t flags)
{
	struct request_queue *q = hctx->queue;
	struct sstf_data *sd = q->elevator->elevator_data;
	unsigned long irqflags;

	spin_lock_irqsave(&sd->lock, irqflags);
	sd->hctx = hctx;

	while (!list_empty(list)) {
		struct request *rq = list_first_entry(list, struct request, queuelist);
		sector_t pos = blk_rq_pos(rq);

		list_del_init(&rq->queuelist);
		list_add_tail(&rq->queuelist, &sd->queue);
		sd->count++;

		/* contabiliza o custo se atendêssemos na ordem de chegada (FCFS) */
		sd->fcfs_total += sstf_dist(pos, sd->fcfs_head);
		sd->fcfs_head = pos;

		sstf_dbg("chegada: setor %llu (fila=%u)\n",
			 (unsigned long long)pos, sd->count);

		if (sd->count >= (unsigned int)queue_size && !sd->ready) {
			sd->ready = true;
			sstf_dbg("fila cheia (%u): despachando\n", sd->count);
		}
	}

	/* enquanto não despacha, (re)arma o timer a partir da última requisição */
	if (!sd->ready && sd->count > 0)
		hrtimer_start(&sd->timer, ms_to_ktime(max_wait_ms), HRTIMER_MODE_REL);

	spin_unlock_irqrestore(&sd->lock, irqflags);
}

/* ---- Despacho: escolhe a requisição mais próxima do cabeçote ---- */
static struct request *sstf_dispatch_request(struct blk_mq_hw_ctx *hctx)
{
	struct sstf_data *sd = hctx->queue->elevator->elevator_data;
	struct request *best = NULL, *rq;
	unsigned long flags;
	sector_t best_dist = 0, pos, old_head;
	unsigned int remaining;
	char dir;

	spin_lock_irqsave(&sd->lock, flags);

	if (!sd->ready || sd->count == 0) {
		spin_unlock_irqrestore(&sd->lock, flags);
		return NULL;
	}

	list_for_each_entry(rq, &sd->queue, queuelist) {
		sector_t d = sstf_dist(blk_rq_pos(rq), sd->head_pos);

		if (!best || d < best_dist) {
			best = rq;
			best_dist = d;
		}
	}

	list_del_init(&best->queuelist);
	sd->count--;

	pos = blk_rq_pos(best);
	old_head = sd->head_pos;
	dir = (pos >= old_head) ? 'R' : 'L';
	sd->sstf_total += best_dist;
	sd->head_pos = pos;
	remaining = sd->count;

	if (sd->count == 0)
		sd->ready = false;	/* fim do lote: volta a acumular */

	spin_unlock_irqrestore(&sd->lock, flags);

	sstf_dbg("atendida: setor %llu dir=%c dist=%llu (restam %u)\n",
		 (unsigned long long)pos, dir,
		 (unsigned long long)best_dist, remaining);

	return best;
}

/* has_work: verdadeiro só quando a fila encheu ou o timer expirou */
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

/* precisa existir, mas não há nada a fazer */
static void sstf_finish_request(struct request *rq)
{
}

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

static int __init sstf_init(void)
{
	pr_info("sstf: registrando (queue_size=%d, max_wait_ms=%d, debug=%d)\n",
		queue_size, max_wait_ms, debug);
	return elv_register(&sstf);
}

static void __exit sstf_exit(void)
{
	elv_unregister(&sstf);
	pr_info("sstf: removido\n");
}

module_init(sstf_init);
module_exit(sstf_exit);
