#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#define SECTOR_SIZE 4096
#define DISK_SZ     1073741824
#define N_OPS       50

void echo(const char *path, const char *string)
{
	FILE *fd = fopen(path, "w");
	fwrite(string, sizeof(char), strlen(string), fd);
	fclose(fd);
}

void drop_caches()
{
	printf("Cleaning disk cache...\n");
	echo("/proc/sys/vm/drop_caches", "3");
}

void cfg_queue(const char *disk)
{
	static char namebuf[256];

	printf("Configuring scheduling queues...\n");

	/* Desabilita agrupamentos */
	sprintf(namebuf, "/sys/block/%s/queue/nomerges", disk);
	echo(namebuf, "2");

	/* define 4Kb para o tamanho máximo de uma requisição */
	sprintf(namebuf, "/sys/block/%s/queue/max_sectors_kb", disk);
	echo(namebuf, "4");

	/* evita que o sistema faça a leitura de mais conteúdo do que o que foi requisitado */
	sprintf(namebuf, "/sys/block/%s/queue/read_ahead_kb", disk);
	echo(namebuf, "0");
}

int main()
{
	static char buf[SECTOR_SIZE];

	printf("Starting sector read example...\n");

	drop_caches();
	cfg_queue("sdb");

	srand(getpid());

	int fd = open("/dev/sdb", O_RDWR);
	if (fd < 0) {
		printf("Failed to open the device...\n");
		return errno;
	}

	for (int i = 0; i < N_OPS; i++) {
		int pos = (rand() % (DISK_SZ / SECTOR_SIZE));

		printf("seek to block %d\n", pos);
		/* Set position */
		lseek(fd, pos * SECTOR_SIZE, SEEK_SET);
		/* Peform read. */
		read(fd, buf, SECTOR_SIZE);
	}
	close(fd);

	return 0;
}
