#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>

static volatile int running = 1;

void *task(void *arg) {
    int tid;
    tid = (int)(long int)arg;
    while (running) {
        /* Caractere 'a' + ID da thread */
        /* Resulta em um ASCII correspondente a a, b, c... */
        putchar('a' + tid);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Uso: %s <num_threads> <timeout_segundos>\n", argv[0]);
        return 1;
    }

    int n = atoi(argv[1]);
    if (n <= 0) {
        fprintf(stderr, "Erro: num_threads deve ser maior que 0\n");
        return 1;
    }

    int timeout = atoi(argv[2]);
    if (timeout <= 0) {
        fprintf(stderr, "Erro: timeout deve ser maior que 0\n");
        return 1;
    }

    pthread_t *threads = malloc(n * sizeof(pthread_t));
    if (threads == NULL)
        return 1;

    for (size_t i = 0; i < n; i++)
        pthread_create(&threads[i], NULL, task, (void *)i);

    sleep(timeout);
    running = 0;

    for (size_t i = 0; i < n; i++)
        pthread_join(threads[i], NULL);

    free(threads);
    printf("\n");
    return 0;
}
