#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

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

static const char *policy_name(uint32_t policy)
{
	switch (policy) {
	case 0:
		return "SCHED_NORMAL";
	case 1:
		return "SCHED_FIFO";
	case 2:
		return "SCHED_RR";
	case 3:
		return "SCHED_BATCH";
	case 5:
		return "SCHED_IDLE";
	case 6:
		return "SCHED_DEADLINE";
	default:
		return "DESCONHECIDA";
	}
}

int main(void)
{
	struct sched_attr attr = {
		.size = sizeof(attr)
	};

	int err = syscall(SYS_sched_getattr, 0, &attr, sizeof(attr), 0);
	if (err < 0) {
		printf("Erro na syscall: %d\n", err);
		return err;
	}

	printf("PID: %d\n", getpid());
	printf("Política: %s (%u)\n", policy_name(attr.sched_policy), attr.sched_policy);
	printf("Nice: %d\n", attr.sched_nice);
	printf("Prioridade RT: %u\n", attr.sched_priority);
	printf("Runtime: %lu ns (%.2f ms)\n", attr.sched_runtime, attr.sched_runtime / 1e6);
	printf("Deadline: %lu ns (%.2f ms)\n", attr.sched_deadline, attr.sched_deadline / 1e6);
	printf("Period: %lu ns (%.2f ms)\n", attr.sched_period, attr.sched_period / 1e6);

	return 0;
}
