#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>

#define DEV_PATH "/dev/pubsub"
#define BUF_SIZE 4096

int main(void)
{
	int fd;
	char line[BUF_SIZE];
	char rbuf[BUF_SIZE];
	ssize_t n;

	fd = open(DEV_PATH, O_RDWR);
	if (fd < 0) {
		perror("open " DEV_PATH);
		return 1;
	}

	fprintf(stdout, "pid=%d\n", (int)getpid());

	while (1) {
		if (!fgets(line, sizeof(line), stdin))
			break;

		size_t len = strlen(line);
		if (len == 0)
			continue;

		if (line[len - 1] == '\n')
			line[len - 1] = '\0';

		if (strcmp(line, "/quit") == 0)
			break;

		if (strcmp(line, "/read") == 0) {
			n = read(fd, rbuf, sizeof(rbuf) - 1);
			if (n < 0)
				perror("read");
			else if (n == 0)
				fprintf(stdout, "(no messages)\n");
			else {
				rbuf[n] = '\0';
				fprintf(stdout, "%s\n", rbuf);
			}
			continue;
		}

		n = write(fd, line, strlen(line));
		if (n < 0)
			perror("write");
	}

	close(fd);
	return 0;
}
