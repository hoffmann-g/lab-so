#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// buffer global compartilhado. current_pos = proxima posicao livre
char *buffer;
int buffer_size;
int current_pos = 0;

// lock via atomic_flag = cmpxchg atomico, sem passar pelo kernel.
// nunca bloqueia, so gira (espera ocupada de verdade)
atomic_flag lock = ATOMIC_FLAG_INIT;

// delay de 1ms, espera ocupada (sem sleep/nanosleep).
// clock_gettime em loop ate passar o tempo alvo
void busy_wait_delay_ms(long ms) {
    struct timespec start, current;
    long target_ns = ms * 1000000L;

    clock_gettime(CLOCK_MONOTONIC_RAW, &start);
    while (1) {
        clock_gettime(CLOCK_MONOTONIC_RAW, &current);
        long elapsed_ns = (current.tv_sec - start.tv_sec) * 1000000000L +
                           (current.tv_nsec - start.tv_nsec);
        if (elapsed_ns >= target_ns)
            break;
    }
}

void *worker(void *arg) {
    int id = *(int *)arg;
    char c = 'A' + id;

    while (1) {
        // espera ocupada pro lock
        while (atomic_flag_test_and_set(&lock)) {
            /* spin */
        }

        // check dentro da secao critica, senao duas threads passam
        // pela checagem juntas e as duas escrevem
        if (current_pos >= buffer_size) {
            atomic_flag_clear(&lock);
            break; // buffer cheio
        }

        buffer[current_pos++] = c;

        atomic_flag_clear(&lock);

        // delay fora do lock de proposito: com o lock livre, o
        // escalonador tem chance de trocar pra outra thread aqui
        busy_wait_delay_ms(1);
    }
    return NULL;
}

// string da cli -> constante SCHED_* do sched.h. -1 = invalida
static int parse_policy(const char *politica) {
    if (!strcmp(politica, "SCHED_OTHER"))
        return SCHED_OTHER;
    if (!strcmp(politica, "SCHED_BATCH"))
        return SCHED_BATCH;
    if (!strcmp(politica, "SCHED_IDLE"))
        return SCHED_IDLE;
    if (!strcmp(politica, "SCHED_RR"))
        return SCHED_RR;
    if (!strcmp(politica, "SCHED_FIFO"))
        return SCHED_FIFO;
    return -1;
}

int main(int argc, char *argv[]) {
    if (argc != 5) {
        fprintf(stderr,
                "Uso: %s <numero_de_threads> <tamanho_do_buffer_global_em_kb> <politica> <prioridade>\n"
                "politicas: SCHED_OTHER SCHED_BATCH SCHED_IDLE SCHED_RR SCHED_FIFO\n",
                argv[0]);
        return 1;
    }

    int num_threads = atoi(argv[1]);
    int buffer_kb = atoi(argv[2]);
    char *politica = argv[3];
    int prioridade = atoi(argv[4]);

    // max 26 = uma letra por thread (A, B, C...)
    if (num_threads <= 0 || num_threads > 26) {
        fprintf(stderr, "Erro: numero_de_threads deve estar entre 1 e 26.\n");
        return 1;
    }
    if (buffer_kb <= 0) {
        fprintf(stderr, "Erro: tamanho_do_buffer_global_em_kb deve ser maior que 0.\n");
        return 1;
    }

    int policy_id = parse_policy(politica);
    if (policy_id < 0) {
        fprintf(stderr, "Erro: politica invalida: %s\n", politica);
        return 1;
    }

    buffer_size = buffer_kb * 1024;
    buffer = malloc(buffer_size);
    if (!buffer) {
        perror("malloc buffer");
        return 1;
    }

    // seta politica na thread principal, antes de criar as workers.
    // pthread_create com atributo NULL = PTHREAD_INHERIT_SCHED,
    // entao as threads criadas depois ja nascem com a mesma config
    struct sched_param param = {.sched_priority = 0};
    if (policy_id == SCHED_RR || policy_id == SCHED_FIFO)
        param.sched_priority = prioridade;

    if (sched_setscheduler(0, policy_id, &param) == -1) {
        perror("sched_setscheduler");
        // aborta em vez de seguir com politica errada sem avisar
        if (policy_id == SCHED_RR || policy_id == SCHED_FIFO)
            fprintf(stderr, "Aviso: politicas de tempo real exigem root ou CAP_SYS_NICE.\n");
        return 1;
    }

    // OTHER/BATCH/IDLE: prioridade estatica fixa em 0, quem varia e o
    // nice value, setado a parte
    if (policy_id == SCHED_OTHER || policy_id == SCHED_BATCH || policy_id == SCHED_IDLE) {
        errno = 0;
        if (nice(prioridade) == -1 && errno != 0)
            perror("nice");
    }

    pthread_t *threads = malloc(num_threads * sizeof(pthread_t));
    int *thread_ids = malloc(num_threads * sizeof(int));
    if (!threads || !thread_ids) {
        perror("malloc");
        return 1;
    }

    for (int i = 0; i < num_threads; i++) {
        thread_ids[i] = i;
        if (pthread_create(&threads[i], NULL, worker, &thread_ids[i]) != 0) {
            perror("pthread_create");
            return 1;
        }
    }

    // buffer cheio = todas as threads ja saíram do loop
    for (int i = 0; i < num_threads; i++)
        pthread_join(threads[i], NULL);

    // buffer cru, do jeito que ficou gravado
    fwrite(buffer, 1, buffer_size, stdout);
    printf("\n\n");

    // contagem = numero de rajadas contiguas de cada char no buffer,
    // NAO o total de bytes escritos (500 bytes seguidos = 1 rajada,
    // nao 500). troca de char = nova rajada = nova vez escalonada.
    // sequencia = essas trocas em ordem (ABCDABCD...)
    int *thread_counts = calloc(num_threads, sizeof(int));
    char *sequence = malloc(buffer_size + 1);
    int seq_len = 0;
    char last = 0;

    for (int i = 0; i < buffer_size; i++) {
        char c = buffer[i];
        if (c != last) {
            sequence[seq_len++] = c;
            thread_counts[c - 'A']++;
            last = c;
        }
    }
    sequence[seq_len] = '\0';

    printf("%s\n", sequence);
    for (int i = 0; i < num_threads; i++)
        printf("%c = %d\n", 'A' + i, thread_counts[i]);

    free(sequence);
    free(thread_counts);
    free(threads);
    free(thread_ids);
    free(buffer);
    return 0;
}
