/*
 * Trabalho 3 -- Aplicação de teste para o escalonador de disco SSTF.
 *
 * Cria N processos (fork) que geram um grande número de requisições de leitura
 * e escrita em regiões aleatórias do disco de testes (/dev/sdb), cobrindo todo
 * o disco, para alimentar o escalonador. Os resultados (comparação SSTF x FCFS)
 * são exibidos pelo módulo no log do kernel (dmesg) quando debug está ativo.
 *
 * Parâmetros (em tempo de execução):
 *   block_size  : tamanho do bloco em bytes (potência de 2)
 *   disk_blocks : tamanho do disco em blocos
 *   n_ops       : número total de operações de E/S
 *   write_pct   : percentual de escritas (0-100), o restante são leituras
 *   min_req     : tamanho mínimo de cada requisição em bytes (<= block_size)
 *   max_req     : tamanho máximo de cada requisição em bytes (<= block_size)
 *   n_procs     : número de processos concorrentes (fork)
 */
#define _GNU_SOURCE	/* necessário para O_DIRECT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/types.h>

/* Device de teste. Padrão /dev/sdb (lab 3.2). Para a fila do escalonador
 * encher de verdade é preciso um disco de fila profunda (nr_requests > 1):
 * use um disco virtio-blk no QEMU e aponte SSTF_DEV=/dev/vda. O IDE (--hdb)
 * tem nr_requests=1 e nunca enche a fila. */
#define DEFAULT_DEV "/dev/sdb"

static void usage(const char *p)
{
	fprintf(stderr,
		"Uso: %s <block_size> <disk_blocks> <n_ops> <write_pct> <min_req> <max_req> <n_procs> [padrao]\n"
		"  block_size : tamanho do bloco em bytes (potencia de 2)\n"
		"  disk_blocks: tamanho do disco em blocos\n"
		"  n_ops      : numero total de operacoes de E/S\n"
		"  write_pct  : percentual de escritas (0-100)\n"
		"  min_req    : tamanho minimo de cada requisicao em bytes (<= block_size)\n"
		"  max_req    : tamanho maximo de cada requisicao em bytes (<= block_size)\n"
		"  n_procs    : numero de processos concorrentes (fork)\n"
		"  [padrao]   : opcional - 'seq' (sequencial) ou 'rand' (aleatorio; padrao)\n", p);
}

static void worker(int idx, int n_procs, long block_size, long disk_blocks,
		   long ops, int write_pct, long min_req, long max_req, int seq)
{
	char *buf;
	int fd;

	/* semente distinta por processo => padrões de acesso independentes */
	srand((unsigned)(getpid() ^ (idx << 16)));

	/* device configurável por SSTF_DEV (padrão DEFAULT_DEV) */
	const char *dev = getenv("SSTF_DEV");
	if (!dev || !*dev)
		dev = DEFAULT_DEV;

	/* O_DIRECT: cada read/write vira uma requisição imediata ao escalonador,
	 * sem passar pela page cache (que absorveria/atrasaria a maioria das ops) */
	fd = open(dev, O_RDWR | O_DIRECT);
	if (fd < 0) {
		perror(dev);
		_exit(1);
	}

	/* O_DIRECT exige buffer alinhado à página e tamanhos múltiplos de 512 */
	size_t bufsz = ((max_req + 4095) / 4096) * 4096;
	if (posix_memalign((void **)&buf, 4096, bufsz) != 0) {
		fprintf(stderr, "posix_memalign falhou\n");
		close(fd);
		_exit(1);
	}
	memset(buf, idx & 0xff, bufsz);

	/* em modo sequencial, cada processo percorre uma faixa própria do disco */
	long base = (long)idx * (disk_blocks / (n_procs > 0 ? n_procs : 1));

	for (long i = 0; i < ops; i++) {
		long blk  = seq ? (base + i) % disk_blocks    /* enderecos crescentes/sequenciais */
				: rand() % disk_blocks;       /* bloco aleatorio em todo o disco */
		long span = (max_req > min_req)
			    ? (min_req + rand() % (max_req - min_req + 1))
			    : min_req;                        /* tamanho variável da requisição */
		off_t off = (off_t)blk * block_size;

		/* O_DIRECT: tamanho múltiplo de 512 (offset já alinhado: bloco * block_size) */
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

	printf("sstf-teste: %d processos x %ld ops "
	       "(bloco=%ld B, disco=%ld blocos, escrita=%d%%, req=%ld..%ld B, padrao=%s)\n",
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

	printf("sstf-teste: concluido. Veja a comparacao SSTF x FCFS no log do kernel (dmesg).\n");
	return 0;
}
