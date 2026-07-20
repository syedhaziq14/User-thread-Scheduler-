#include "uthread.h"
#include <stdio.h>
#include <string.h>

/*
 * Test: Mutex correctness
 *
 * 4 threads each increment a shared counter 100,000 times.
 * A yield between the read and write of the counter simulates
 * the kind of preemption that would happen in a real OS.
 *
 * - "nolock" mode: no mutex, the yield causes lost updates (race)
 * - "lock"   mode: mutex protects the critical section, counter
 *                   is exactly 400,000 every time
 *
 * Usage:
 *   ./test_mutex nolock
 *   ./test_mutex lock
 */

#define ITERATIONS   100000
#define NUM_THREADS  4
#define YIELD_EVERY  10   /* yield between read & write every N iterations */

static int counter = 0;
static uthread_mutex_t mutex;
static int use_mutex = 0;
static int threads_done = 0;

void worker(void *arg) {
    (void)arg;

    for (int i = 0; i < ITERATIONS; i++) {
        if (use_mutex) uthread_mutex_lock(&mutex);

        int tmp = counter;
        if (i % YIELD_EVERY == 0) uthread_yield();
        counter = tmp + 1;

        if (use_mutex) uthread_mutex_unlock(&mutex);
    }

    threads_done++;
    if (threads_done == NUM_THREADS) {
        int expected = NUM_THREADS * ITERATIONS;
        printf("Final counter: %d (expected %d) %s\n",
               counter, expected,
               counter == expected ? "PASS" : "FAIL");
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <nolock|lock>\n", argv[0]);
        return 1;
    }

    use_mutex = (strcmp(argv[1], "lock") == 0);
    uthread_init();
    uthread_mutex_init(&mutex);

    printf("%-15s ", use_mutex ? "[With Mutex]" : "[Without Mutex]");
    fflush(stdout);

    static int ids[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {
        ids[i] = i + 1;
        uthread_create(worker, &ids[i]);
    }

    uthread_exit();
    return 0;
}
