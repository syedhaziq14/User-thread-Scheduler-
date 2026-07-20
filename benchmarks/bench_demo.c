#include "uthread.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

#define NSEC_PER_SEC 1000000000L
#define NUM_THREADS  15000

static double get_time_diff(struct timespec start, struct timespec end) {
    double s = start.tv_sec + (double)start.tv_nsec / NSEC_PER_SEC;
    double e = end.tv_sec + (double)end.tv_nsec / NSEC_PER_SEC;
    return e - s;
}

static volatile int done_count = 0;

static void ut_worker(void *arg) {
    (void)arg;
    __sync_fetch_and_add(&done_count, 1);
}

static void wait_for_ut(void) {
    while (__sync_fetch_and_add(&done_count, 0) < NUM_THREADS)
        uthread_yield();
}

static void* pt_worker(void *arg) {
    (void)arg;
    return NULL;
}

static void run_uthread_bench(int pipe_fd) {
    struct timespec start, end;
    uthread_init();

    done_count = 0;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < NUM_THREADS; i++) {
        uthread_create(ut_worker, NULL);
    }
    wait_for_ut();
    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed = get_time_diff(start, end);
    write(pipe_fd, &elapsed, sizeof(elapsed));
    close(pipe_fd);
    exit(0);
}

static void run_pthread_bench(int pipe_fd) {
    struct timespec start, end;
    pthread_t *threads = malloc(sizeof(pthread_t) * NUM_THREADS);

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_create(&threads[i], NULL, pt_worker, NULL);
    }
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed = get_time_diff(start, end);
    write(pipe_fd, &elapsed, sizeof(elapsed));
    close(pipe_fd);
    free(threads);
    exit(0);
}

static double run_child(void (*fn)(int)) {
    int fd[2];
    pipe(fd);
    pid_t pid = fork();
    if (pid == 0) {
        close(fd[0]);
        fn(fd[1]);
    } else {
        close(fd[1]);
        double elapsed;
        read(fd[0], &elapsed, sizeof(elapsed));
        close(fd[0]);
        wait(NULL);
        return elapsed;
    }
    return 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("=========================================================================\n");
    printf("         CLASS DEMO: MASSIVE CONCURRENCY (SPAWNING %d THREADS)\n", NUM_THREADS);
    printf("=========================================================================\n\n");

    printf("1. Running Native OS Threads (pthreads)...\n");
    double t_pthread = run_child(run_pthread_bench);
    printf("  => %.4f seconds\n\n", t_pthread);

    printf("2. Running Green Threads (uthread)...\n");
    double t_uthread = run_child(run_uthread_bench);
    printf("  => %.4f seconds\n\n", t_uthread);

    printf("=========================================================================\n");
    printf("RESULTS: Your uthread library is %.2fx FASTER!\n", t_pthread / t_uthread);
    printf("=========================================================================\n");

    return 0;
}
