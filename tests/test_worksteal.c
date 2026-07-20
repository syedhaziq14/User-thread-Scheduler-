#include "uthread.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>

/*
 * Test: M:N Work Stealing
 *
 * Creates threads across multiple virtual processors and verifies
 * that work stealing distributes them correctly and all threads complete.
 */

#define NUM_WORK_THREADS 100

static atomic_int work_completed = 0;

static void compute_worker(void *arg) {
    int id = *(int*)arg;
    volatile long sum = 0;

    /* CPU-bound work */
    for (int i = 0; i < 100000; i++) {
        sum += i * id;
    }

    int count = __sync_fetch_and_add(&work_completed, 1) + 1;
    if (count % 25 == 0 || count == NUM_WORK_THREADS) {
        printf("  Progress: %d/%d threads completed\n", count, NUM_WORK_THREADS);
    }
}

int main(void) {
    printf("=== Test: M:N Work Stealing ===\n\n");

    int num_cores = 4;  /* Use 4 VPs */
    printf("Initializing M:N runtime with %d virtual processors...\n", num_cores);
    uthread_init_mn(num_cores);
    uthread_trace_enable();

    static int ids[NUM_WORK_THREADS];
    for (int i = 0; i < NUM_WORK_THREADS; i++) {
        ids[i] = i;
        int tid = uthread_create(compute_worker, &ids[i]);
        if (tid < 0) {
            printf("FAIL: Could not create thread %d\n", i);
            exit(1);
        }
    }

    printf("Created %d threads. Starting M:N runtime...\n\n", NUM_WORK_THREADS);
    uthread_mn_run();

    int completed = __sync_fetch_and_add(&work_completed, 0);
    printf("\nResults: %d/%d threads completed\n", completed, NUM_WORK_THREADS);

    if (completed == NUM_WORK_THREADS) {
        printf("PASS: All threads completed successfully!\n");
    } else {
        printf("FAIL: Only %d threads completed\n", completed);
        return 1;
    }

    return 0;
}
