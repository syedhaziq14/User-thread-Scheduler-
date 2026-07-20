#include "uthread.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * Benchmark: M:N Work Stealing Performance
 *
 * Uses fork() to run each benchmark in an isolated process (since uthread_exit
 * terminates the process). Compares:
 *   1. Single-core uthread (1:N)
 *   2. M:N uthread with work stealing
 *   3. Raw pthreads
 *   4. Context switch latency overhead
 */

#define NSEC_PER_SEC 1000000000L
#define PRIME_LIMIT  50000
#define NUM_WORKERS  16
#define CS_ITERS     50000

static double get_time_diff(struct timespec start, struct timespec end) {
    double s = start.tv_sec + (double)start.tv_nsec / NSEC_PER_SEC;
    double e = end.tv_sec + (double)end.tv_nsec / NSEC_PER_SEC;
    return e - s;
}

/* ========== CPU-Bound: Count primes ========== */

static int count_primes(int limit) {
    int count = 0;
    for (int n = 2; n <= limit; n++) {
        int is_prime = 1;
        for (int d = 2; d * d <= n; d++) {
            if (n % d == 0) { is_prime = 0; break; }
        }
        count += is_prime;
    }
    return count;
}

/* ========== uthread workers ========== */

static volatile int ut_done = 0;

static void ut_prime_worker(void *arg) {
    (void)arg;
    count_primes(PRIME_LIMIT);
    __sync_fetch_and_add(&ut_done, 1);
}

static void wait_for_ut(void) {
    while (__sync_fetch_and_add(&ut_done, 0) < NUM_WORKERS)
        uthread_yield();
}

/* ========== pthread workers ========== */

static void* pt_prime_worker(void *arg) {
    (void)arg;
    count_primes(PRIME_LIMIT);
    return NULL;
}

/* ========== Context switch workers ========== */

static volatile int cs_done = 0;

static void cs_worker(void *arg) {
    (void)arg;
    for (int i = 0; i < CS_ITERS; i++)
        uthread_yield();
    __sync_fetch_and_add(&cs_done, 1);
}

/* ========== Individual benchmarks (run in forked children) ========== */

/* Returns elapsed time via a pipe to the parent */
static void run_1n_bench(int pipe_fd) {
    struct timespec start, end;
    uthread_init();

    ut_done = 0;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < NUM_WORKERS; i++)
        uthread_create(ut_prime_worker, NULL);
    wait_for_ut();
    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed = get_time_diff(start, end);
    write(pipe_fd, &elapsed, sizeof(elapsed));
    close(pipe_fd);
    exit(0);
}

static void run_mn_bench(int pipe_fd, int num_vps) {
    struct timespec start, end;
    uthread_init_mn(num_vps);

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < NUM_WORKERS; i++)
        uthread_create(ut_prime_worker, NULL);
    uthread_mn_run();
    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed = get_time_diff(start, end);
    write(pipe_fd, &elapsed, sizeof(elapsed));
    close(pipe_fd);
    exit(0);
}

static void run_pt_bench(int pipe_fd) {
    struct timespec start, end;
    pthread_t threads[NUM_WORKERS];

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < NUM_WORKERS; i++)
        pthread_create(&threads[i], NULL, pt_prime_worker, NULL);
    for (int i = 0; i < NUM_WORKERS; i++)
        pthread_join(threads[i], NULL);
    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed = get_time_diff(start, end);
    write(pipe_fd, &elapsed, sizeof(elapsed));
    close(pipe_fd);
    exit(0);
}

static void run_cs_bench(int pipe_fd) {
    struct timespec start, end;
    uthread_init();

    cs_done = 0;

    clock_gettime(CLOCK_MONOTONIC, &start);
    uthread_create(cs_worker, NULL);
    uthread_create(cs_worker, NULL);

    /* Wait for both to finish */
    while (__sync_fetch_and_add(&cs_done, 0) < 2) {
        uthread_yield();
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed = get_time_diff(start, end);
    write(pipe_fd, &elapsed, sizeof(elapsed));
    close(pipe_fd);
    exit(0);
}

static double fork_and_run(void (*bench_fn)(int), int extra_arg) {
    int pipefd[2];
    pipe(pipefd);
    fflush(stdout);

    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        if (bench_fn == (void(*)(int))run_mn_bench) {
            run_mn_bench(pipefd[1], extra_arg);
        } else {
            bench_fn(pipefd[1]);
        }
        _exit(0);
    }

    close(pipefd[1]);
    double result = 0;
    read(pipefd[0], &result, sizeof(result));
    close(pipefd[0]);
    waitpid(pid, NULL, 0);
    return result;
}

int main(void) {
    printf("=========================================================================\n");
    printf("         M:N WORK STEALING BENCHMARK\n");
    printf("=========================================================================\n");
    printf("Task: Count primes up to %d  |  Workers: %d\n\n", PRIME_LIMIT, NUM_WORKERS);

    /* Benchmark 1: Single-core */
    printf("Running: Single-core uthread (1:N)...\n");
    double t_1n = fork_and_run(run_1n_bench, 0);
    printf("  => %.4f seconds\n\n", t_1n);

    /* Benchmark 2: M:N with 4 VPs */
    int num_vps = 4;
    printf("Running: M:N uthread (%d VPs) with work stealing...\n", num_vps);
    double t_mn = fork_and_run((void(*)(int))run_mn_bench, num_vps);
    printf("  => %.4f seconds\n\n", t_mn);

    /* Benchmark 3: pthreads */
    printf("Running: Raw pthreads...\n");
    double t_pt = fork_and_run(run_pt_bench, 0);
    printf("  => %.4f seconds\n\n", t_pt);

    /* Benchmark 4: Context switch latency */
    printf("Running: Context switch latency (%d switches)...\n", CS_ITERS * 2);
    double t_cs = fork_and_run(run_cs_bench, 0);
    printf("  => %.4f seconds (%.0f ns/switch)\n\n",
           t_cs, (t_cs * 1e9) / (CS_ITERS * 2));

    /* Results table */
    printf("=========================================================================\n");
    printf("%-25s | %-12s | %-10s\n", "Configuration", "Time (s)", "Speedup");
    printf("-------------------------------------------------------------------------\n");
    printf("%-25s | %-12.4f | %-10s\n", "uthread 1:N", t_1n, "1.00x");
    if (t_mn > 0)
        printf("%-25s | %-12.4f | %-10.2fx\n", "uthread M:N (4 VPs)", t_mn, t_1n / t_mn);
    if (t_pt > 0)
        printf("%-25s | %-12.4f | %-10.2fx\n", "pthreads", t_pt, t_1n / t_pt);
    printf("%-25s | %-12.4f | %-10s\n", "Context Switch Latency", t_cs, "-");
    printf("=========================================================================\n");

    return 0;
}
