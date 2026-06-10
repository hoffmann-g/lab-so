/*
 * Trabalho 3 -- Test application for the SSTF disk scheduler.
 *
 * Creates N processes (fork) that generate a large number of read and write
 * requests over random regions of the test disk, covering the whole disk, to
 * feed the scheduler. The results (SSTF vs FCFS comparison) are printed by the
 * module in the kernel log (dmesg) when debug is enabled.
 *
 * Parameters (run time):
 *   block_size  : block size in bytes (power of two)
 *   disk_blocks : disk size in blocks
 *   n_ops       : total number of I/O operations
 *   write_pct   : write percentage (0-100); the rest are reads
 *   min_req     : minimum size of each request in bytes (<= block_size)
 *   max_req     : maximum size of each request in bytes (<= block_size)
 *   n_procs     : number of concurrent processes (fork)
 */
#define _GNU_SOURCE	/* needed for O_DIRECT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/types.h>

/* Test device. Default /dev/sdb (--hdb sdb.bin, lab 3.2). Override via SSTF_DEV.
 * For the scheduler queue to fill, raise the request-queue depth AFTER attaching
 * the scheduler: `echo 128 > /sys/block/sdb/queue/nr_requests` (a plain IDE disk
 * starts at nr_requests=1). The sstf-bench script does this automatically. */
#define DEFAULT_DEV "/dev/sdb"

static void usage(const char *p)
{
	fprintf(stderr,
		"Usage: %s <block_size> <disk_blocks> <n_ops> <write_pct> <min_req> <max_req> <n_procs> [pattern]\n"
		"  block_size : block size in bytes (power of two)\n"
		"  disk_blocks: disk size in blocks\n"
		"  n_ops      : total number of I/O operations\n"
		"  write_pct  : write percentage (0-100)\n"
		"  min_req    : minimum size of each request in bytes (<= block_size)\n"
		"  max_req    : maximum size of each request in bytes (<= block_size)\n"
		"  n_procs    : number of concurrent processes (fork)\n"
		"  [pattern]  : optional - 'seq' (sequential) or 'rand' (random; default)\n", p);
}

static void worker(int idx, int n_procs, long block_size, long disk_blocks,
		   long ops, int write_pct, long min_req, long max_req, int seq)
{
	char *buf;
	int fd;

	/* distinct seed per process => independent access patterns */
	srand((unsigned)(getpid() ^ (idx << 16)));

	/* device configurable via SSTF_DEV (default DEFAULT_DEV) */
	const char *dev = getenv("SSTF_DEV");
	if (!dev || !*dev)
		dev = DEFAULT_DEV;

	/* O_DIRECT: each read/write becomes an immediate request to the scheduler,
	 * bypassing the page cache (which would absorb/delay most of the ops) */
	fd = open(dev, O_RDWR | O_DIRECT);
	if (fd < 0) {
		perror(dev);
		_exit(1);
	}

	/* O_DIRECT requires a page-aligned buffer and sizes multiple of 512 */
	size_t bufsz = ((max_req + 4095) / 4096) * 4096;
	if (posix_memalign((void **)&buf, 4096, bufsz) != 0) {
		fprintf(stderr, "posix_memalign failed\n");
		close(fd);
		_exit(1);
	}
	memset(buf, idx & 0xff, bufsz);

	/* in sequential mode, each process scans its own range of the disk */
	long base = (long)idx * (disk_blocks / (n_procs > 0 ? n_procs : 1));

	for (long i = 0; i < ops; i++) {
		long blk  = seq ? (base + i) % disk_blocks    /* increasing/sequential addresses */
				: rand() % disk_blocks;       /* random block over the whole disk */
		long span = (max_req > min_req)
			    ? (min_req + rand() % (max_req - min_req + 1))
			    : min_req;                        /* variable request size */
		off_t off = (off_t)blk * block_size;

		/* O_DIRECT: size multiple of 512 (offset already aligned: block * block_size) */
		span = (span / 512) * 512;
		if (span < 512)
			span = 512;

		lseek(fd, off, SEEK_SET);
		if ((rand() % 100) < write_pct)
			(void)!write(fd, buf, span);
		else
			(void)!read(fd, buf, span);
	}

	fsync(fd);
	free(buf);
	close(fd);
	_exit(0);
}

int main(int argc, char *argv[])
{
	if (argc != 8 && argc != 9) {
		usage(argv[0]);
		return 1;
	}

	long block_size  = atol(argv[1]);
	long disk_blocks = atol(argv[2]);
	long n_ops       = atol(argv[3]);
	int  write_pct   = atoi(argv[4]);
	long min_req     = atol(argv[5]);
	long max_req     = atol(argv[6]);
	int  n_procs     = atoi(argv[7]);
	int  seq         = (argc == 9 && strcmp(argv[8], "seq") == 0);

	if (block_size <= 0 || disk_blocks <= 0 || n_ops <= 0 ||
	    write_pct < 0 || write_pct > 100 ||
	    min_req <= 0 || max_req <= 0 || min_req > max_req ||
	    max_req > block_size || n_procs <= 0) {
		usage(argv[0]);
		return 1;
	}

	long ops_per_proc = n_ops / n_procs;
	if (ops_per_proc < 1)
		ops_per_proc = 1;

	printf("sstf-teste: %d processes x %ld ops "
	       "(block=%ld B, disk=%ld blocks, writes=%d%%, req=%ld..%ld B, pattern=%s)\n",
	       n_procs, ops_per_proc, block_size, disk_blocks, write_pct, min_req, max_req,
	       seq ? "seq" : "rand");

	for (int i = 0; i < n_procs; i++) {
		pid_t pid = fork();

		if (pid < 0) {
			perror("fork");
			return 1;
		}
		if (pid == 0)
			worker(i, n_procs, block_size, disk_blocks, ops_per_proc,
			       write_pct, min_req, max_req, seq);
	}

	for (int i = 0; i < n_procs; i++)
		wait(NULL);

	printf("sstf-teste: done. See the SSTF vs FCFS comparison in the kernel log (dmesg).\n");
	return 0;
}
