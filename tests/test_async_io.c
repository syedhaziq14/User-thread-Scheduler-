#include "uthread.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/*
 * Test: Async I/O — uthread_sleep()
 *
 * Creates multiple threads that sleep for different durations.
 * Verifies that:
 *  1. Sleeping threads don't block other threads from running.
 *  2. Threads wake up after approximately the right delay.
 *  3. The scheduler doesn't deadlock when all threads are sleeping.
 */

static void sleeper(void *arg) {
    int id = *(int*)arg;
    unsigned int ms = (id + 1) * 50;  /* 50ms, 100ms, 150ms, ... */

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    printf("  Thread %d: sleeping for %u ms...\n", id, ms);
    uthread_sleep(ms);

    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (end.tv_sec - start.tv_sec) * 1000.0 +
                     (end.tv_nsec - start.tv_nsec) / 1000000.0;

    printf("  Thread %d: woke up after %.1f ms (expected ~%u ms)\n",
           id, elapsed, ms);
}

static void busy_worker(void *arg) {
    int id = *(int*)arg;
    printf("  Busy thread %d: doing work while others sleep...\n", id);
    for (volatile int i = 0; i < 1000000; i++);
    printf("  Busy thread %d: done!\n", id);
}

int main(void) {
    printf("=== Test: Async I/O (uthread_sleep) ===\n\n");

    uthread_init();
    uthread_trace_enable();

    static int ids[8];
    for (int i = 0; i < 8; i++) ids[i] = i;

    /* Create sleeping threads */
    for (int i = 0; i < 4; i++) {
        uthread_create(sleeper, &ids[i]);
    }

    /* Create busy threads that should run while sleepers are blocked */
    for (int i = 4; i < 8; i++) {
        uthread_create(busy_worker, &ids[i]);
    }

    printf("Main thread exiting — scheduler takes over.\n\n");
    uthread_exit();

    return 0;
}
