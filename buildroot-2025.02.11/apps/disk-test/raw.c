#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

int main()
{
	static char *buf = "Hello World!";

	int fd = open("/dev/sdb", O_RDWR);
	if (fd < 0) {
		perror("Failed to open the device...");
		return errno;
	}

	/* Posicionar-se no inicio do disco. */
	lseek(fd, 0, SEEK_SET);

	/* executa escrita. */
	write(fd, buf, strlen(buf));

	close(fd);

	return 0;
}
