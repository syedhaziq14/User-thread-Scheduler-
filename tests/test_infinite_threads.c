#include "uthread.h"
#include <stdio.h>
#include <stdlib.h>

/*
 * Test: Infinite Dynamic Threads
 *
 * Creates 10,000 threads to verify the dynamic allocation system works
 * correctly without any hardcoded limits. Each thread increments a shared
 * counter and exits.
 */

#define NUM_THREADS 10000

static volatile int completed = 0;

static void worker(void *arg) {
    (void)arg;
    __sync_fetch_and_add(&completed, 1);
}

int main(void) {
    printf("=== Test: Infinite Dynamic Threads ===\n");
    printf("Creating %d threads...\n", NUM_THREADS);

    uthread_init();

    for (int i = 0; i < NUM_THREADS; i++) {
        int tid = uthread_create(worker, NULL);
        if (tid < 0) {
            printf("FAIL: Could not create thread %d\n", i);
            exit(1);
        }
        /* Periodically report progress */
        if ((i + 1) % 2000 == 0) {
            printf("  Created %d threads so far...\n", i + 1);
        }
    }

    printf("All %d threads created. Waiting for completion...\n", NUM_THREADS);
    uthread_exit();

    /* Not reached — uthread_exit() exits the process */
    return 0;
}
