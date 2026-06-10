/*
 * Lab 3.2 -- Atividades: comparação de escalonadores de E/S.
 *
 * Modifica o sector-read.c (reusa os mesmos helpers echo/drop_caches/cfg_queue):
 *   (1) gera um número considerável de acessos e compara as estatísticas de
 *       /sys/block/sdb/stat entre os escalonadores disponíveis;
 *   (2) faz operações de leitura E escrita, variando a quantidade de dados;
 *   (3) testa automaticamente todos os escalonadores e imprime os resultados
 *       formatados de maneira legível.
 *
 * Significado das colunas de /sys/block/<dev>/stat (Documentation/block/stat.rst):
 *    1 read I/Os     2 read merges    3 read sectors    4 read ticks (ms)
 *    5 write I/Os    6 write merges   7 write sectors   8 write ticks (ms)
 *    9 in flight    10 io ticks (ms) 11 time in queue (ms)
 *
 * Obs.: cfg_queue() (do guia) define nomerges=2, o que zera as colunas de
 * merges. Para observar merges, basta relaxar essa configuração.
 */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#define SECTOR_SIZE 4096
#define DISK_SZ     1073741824
#define N_OPS       200000	/* "centenas de milhares" de acessos */
#define MAX_REQ_SEC 8		/* varia a quantidade de dados: 1..8 setores por operação */
#define DISK        "sdb"

/* --- helpers iguais aos do sector-read.c --- */

void echo(const char *path, const char *string)
{
	FILE *fd = fopen(path, "w");
	if (!fd)
		return;
	fwrite(string, sizeof(char), strlen(string), fd);
	fclose(fd);
}

void drop_caches()
{
	echo("/proc/sys/vm/drop_caches", "3");
}

void cfg_queue(const char *disk)
{
	static char namebuf[256];

	/* Desabilita agrupamentos */
	sprintf(namebuf, "/sys/block/%s/queue/nomerges", disk);
	echo(namebuf, "2");

	/* define 4Kb para o tamanho máximo de uma requisição */
	sprintf(namebuf, "/sys/block/%s/queue/max_sectors_kb", disk);
	echo(namebuf, "4");

	/* evita que o sistema faça a leitura de mais conteúdo do que o requisitado */
	sprintf(namebuf, "/sys/block/%s/queue/read_ahead_kb", disk);
	echo(namebuf, "0");
}

/* --- estatísticas e escalonadores via sysfs --- */

struct stat_fields {
	unsigned long long rd_ios, rd_merges, rd_sec, rd_ticks;
	unsigned long long wr_ios, wr_merges, wr_sec, wr_ticks;
};

int read_stat(const char *disk, struct stat_fields *s)
{
	static char path[256];
	unsigned long long inflight, ioticks, tinq;

	sprintf(path, "/sys/block/%s/stat", disk);
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;

	int n = fscanf(f, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
		       &s->rd_ios, &s->rd_merges, &s->rd_sec, &s->rd_ticks,
		       &s->wr_ios, &s->wr_merges, &s->wr_sec, &s->wr_ticks,
		       &inflight, &ioticks, &tinq);
	fclose(f);
	return (n >= 8) ? 0 : -1;
}

/* lê /sys/block/<dev>/queue/scheduler ("none [mq-deadline] kyber") */
int list_scheds(const char *disk, char names[][32], int max)
{
	static char path[256], line[256];

	sprintf(path, "/sys/block/%s/queue/scheduler", disk);
	FILE *f = fopen(path, "r");
	if (!f)
		return 0;
	if (!fgets(line, sizeof(line), f)) {
		fclose(f);
		return 0;
	}
	fclose(f);

	int c = 0;
	char *tok = strtok(line, " \t\n");
	while (tok && c < max) {
		/* o escalonador ativo aparece entre colchetes: [nome] */
		if (tok[0] == '[') {
			tok++;
			char *end = strchr(tok, ']');
			if (end)
				*end = '\0';
		}
		strncpy(names[c], tok, 31);
		names[c][31] = '\0';
		c++;
		tok = strtok(NULL, " \t\n");
	}
	return c;
}

void set_sched(const char *disk, const char *name)
{
	static char path[256];

	sprintf(path, "/sys/block/%s/queue/scheduler", disk);
	echo(path, name);
}

void run_workload(int fd, char *buf)
{
	for (int i = 0; i < N_OPS; i++) {
		int nsec = 1 + (rand() % MAX_REQ_SEC);
		int pos  = rand() % (DISK_SZ / SECTOR_SIZE - MAX_REQ_SEC);
		size_t len = (size_t)nsec * SECTOR_SIZE;

		lseek(fd, (long)pos * SECTOR_SIZE, SEEK_SET);

		/* mistura leituras e escritas (~1/3 escritas) */
		if (rand() % 3 == 0)
			write(fd, buf, len);
		else
			read(fd, buf, len);
	}
	fsync(fd);
}

int main()
{
	static char buf[MAX_REQ_SEC * SECTOR_SIZE];
	char scheds[8][32];

	int fd = open("/dev/" DISK, O_RDWR);
	if (fd < 0) {
		printf("Failed to open the device...\n");
		return errno;
	}

	int ns = list_scheds(DISK, scheds, 8);
	if (ns == 0) {
		printf("Não foi possível listar escalonadores de /dev/%s\n", DISK);
		close(fd);
		return 1;
	}

	printf("Comparando %d escalonadores em /dev/%s (%d operações cada)\n\n", ns, DISK, N_OPS);
	printf("%-12s %8s %8s %9s %8s %8s %8s %9s %8s\n",
	       "scheduler", "rd_ios", "rd_mrg", "rd_sec", "rd_tick",
	       "wr_ios", "wr_mrg", "wr_sec", "wr_tick");

	for (int i = 0; i < ns; i++) {
		struct stat_fields before, after;

		set_sched(DISK, scheds[i]);
		drop_caches();
		cfg_queue(DISK);

		/* mesma sequência de acessos para todos -> comparação justa */
		srand(1234);

		read_stat(DISK, &before);
		run_workload(fd, buf);
		read_stat(DISK, &after);

		printf("%-12s %8llu %8llu %9llu %8llu %8llu %8llu %9llu %8llu\n",
		       scheds[i],
		       after.rd_ios - before.rd_ios,
		       after.rd_merges - before.rd_merges,
		       after.rd_sec - before.rd_sec,
		       after.rd_ticks - before.rd_ticks,
		       after.wr_ios - before.wr_ios,
		       after.wr_merges - before.wr_merges,
		       after.wr_sec - before.wr_sec,
		       after.wr_ticks - before.wr_ticks);
	}

	close(fd);
	return 0;
}
