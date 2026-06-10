#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/syscall.h>
#include <errno.h>
#include <sched.h>

#define SCHED_DEADLINE 6

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

static void print_usage(const char *prog)
{
	fprintf(stderr, "Uso: %s <runtime_ms> <deadline_ms> <period_ms> <trabalho_ms>\n", prog);
}

int main(int argc, char *argv[])
{
	if (argc != 5) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}

	char *endptr = NULL;
	long runtime_ms = strtol(argv[1], &endptr, 10);
	if (*argv[1] == '\0' || *endptr != '\0' || runtime_ms <= 0) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}

	long deadline_ms = strtol(argv[2], &endptr, 10);
	if (*argv[2] == '\0' || *endptr != '\0' || deadline_ms <= 0) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}

	long period_ms = strtol(argv[3], &endptr, 10);
	if (*argv[3] == '\0' || *endptr != '\0' || period_ms <= 0) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}

	long work_ms = strtol(argv[4], &endptr, 10);
	if (*argv[4] == '\0' || *endptr != '\0' || work_ms <= 0) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}

	struct sched_attr attr = {
		.size = sizeof(attr),
		.sched_policy = SCHED_DEADLINE,
		.sched_runtime = MS_TO_NS(runtime_ms),
		.sched_deadline = MS_TO_NS(deadline_ms),
		.sched_period = MS_TO_NS(period_ms)
	};

	printf("SCHED_DEADLINE | r=%ld | d=%ld | p=%ld | w=%ld\n", runtime_ms, deadline_ms, period_ms, work_ms);

	int err = syscall(SYS_sched_setattr, 0, &attr, 0);
	if (err < 0) {
		if (errno == EPERM)
			printf("Sem permissao para configurar SCHED_DEADLINE\n");
		else if (errno == EINVAL)
			printf("Utilização de CPU >1. Reduza runtime ou aumente period.\n");
		else
			printf("Erro desconhecido ao configurar SCHED_DEADLINE: %d\n", errno);

		return errno;
	}

	printf("Política aplicada com sucesso. Iniciando loop periódico...\n\n");

	while (1) {
		static int njob = 0;
		struct timespec then;
		clock_gettime(CLOCK_MONOTONIC, &then);

		do_work((int)work_ms);

		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);

		long elapsed = (now.tv_sec - then.tv_sec) * 1000 + (now.tv_nsec - then.tv_nsec) / 1000000;

		printf("Job %d concluído em %3ld ms\n", ++njob, elapsed);
		if (elapsed > period_ms)
			printf("Ocorreu throttling!\n");

		sched_yield();
	}

	return 0;
}
