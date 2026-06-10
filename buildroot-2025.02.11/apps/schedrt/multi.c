/*
 * Lab 3.1 -- Atividade: 3 threads com SCHED_DEADLINE.
 *
 * Utilização total = Σ(runtime/period):
 *   A: 10/50  = 0,200
 *   B:  5/100 = 0,050
 *   C:  3/200 = 0,015
 *   U total   = 0,265  (<= 1  => conjunto escalonável)
 *
 * Cada thread configura seus próprios atributos de deadline com sched_setattr
 * (pid=0 => thread chamadora), executa 20 jobs imprimindo seu TID (gettid())
 * e o tempo de cada job, e chama sched_yield() ao final de cada job.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sched.h>
#include <errno.h>
#include <pthread.h>
#include <sys/syscall.h>

#define SCHED_DEADLINE 6
#define NJOBS          20

struct sched_attr {
	uint32_t size;
	uint32_t sched_policy;
	uint64_t sched_flags;
	int32_t  sched_nice;
	uint32_t sched_priority;
	uint64_t sched_runtime;
	uint64_t sched_deadline;
	uint64_t sched_period;
};

#define MS_TO_NS(ms) ((uint64_t)(ms) * 1000000ULL)

/* glibc não exporta gettid() em todas as versões: usamos a syscall diretamente */
static pid_t get_tid(void)
{
	return (pid_t)syscall(SYS_gettid);
}

struct task_params {
	const char *label;
	long runtime_ms;
	long deadline_ms;
	long period_ms;
	long work_ms;
};

/* Queima CPU por ~ms de tempo de CPU da própria thread (WCET simulado) */
static void do_work(int ms)
{
	struct timespec then;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &then);
	long elapsed;
	do {
		volatile int x = 0;
		for (int i = 0; i < 1000; i++) x++;
		struct timespec now;
		clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
		elapsed = (now.tv_sec - then.tv_sec) * 1000000000L + (now.tv_nsec - then.tv_nsec);
	} while (elapsed < (long)MS_TO_NS(ms));
}

static void *thread_fn(void *arg)
{
	struct task_params *p = (struct task_params *)arg;
	pid_t tid = get_tid();

	struct sched_attr attr = {
		.size          = sizeof(attr),
		.sched_policy  = SCHED_DEADLINE,
		.sched_runtime = MS_TO_NS(p->runtime_ms),
		.sched_deadline = MS_TO_NS(p->deadline_ms),
		.sched_period  = MS_TO_NS(p->period_ms)
	};

	/* aplica a política passando o TID da thread (dica do enunciado) */
	if (syscall(SYS_sched_setattr, tid, &attr, 0) < 0) {
		if (errno == EPERM)
			fprintf(stderr, "[%s tid=%d] Sem permissao para SCHED_DEADLINE\n", p->label, tid);
		else if (errno == EINVAL)
			fprintf(stderr, "[%s tid=%d] Utilização de CPU >1 (EINVAL)\n", p->label, tid);
		else
			fprintf(stderr, "[%s tid=%d] Erro %d ao configurar SCHED_DEADLINE\n", p->label, tid, errno);
		return NULL;
	}

	printf("[%s tid=%d] SCHED_DEADLINE r=%ld d=%ld p=%ld w=%ld -- iniciando %d jobs\n",
	       p->label, tid, p->runtime_ms, p->deadline_ms, p->period_ms, p->work_ms, NJOBS);

	for (int njob = 1; njob <= NJOBS; njob++) {
		struct timespec then;
		clock_gettime(CLOCK_MONOTONIC, &then);

		do_work((int)p->work_ms);

		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		long elapsed = (now.tv_sec - then.tv_sec) * 1000 + (now.tv_nsec - then.tv_nsec) / 1000000;

		printf("[%s tid=%d] Job %2d concluído em %3ld ms\n", p->label, tid, njob, elapsed);

		/* informa ao kernel que o job terminou; suspende até o próximo período */
		sched_yield();
	}

	return NULL;
}

int main(void)
{
	struct task_params tasks[] = {
		{ "A", 10, 30,  50, 8 },
		{ "B",  5, 20, 100, 4 },
		{ "C",  3, 40, 200, 2 },
	};
	const int n = sizeof(tasks) / sizeof(tasks[0]);
	pthread_t th[3];

	printf("Atividade 3.1 -- 3 threads SCHED_DEADLINE (U total = 0,265)\n\n");

	for (int i = 0; i < n; i++) {
		if (pthread_create(&th[i], NULL, thread_fn, &tasks[i]) != 0) {
			perror("pthread_create");
			return EXIT_FAILURE;
		}
	}

	for (int i = 0; i < n; i++)
		pthread_join(th[i], NULL);

	printf("\nTodas as threads concluíram seus jobs.\n");
	return 0;
}
