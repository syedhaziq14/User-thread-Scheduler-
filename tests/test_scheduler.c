#include "uthread.h"
#include <stdio.h>

/*
 * Test: Pluggable Scheduling Policies
 *
 * Creates 6 threads with mixed priorities, runs them once under
 * round-robin and once under priority scheduling, and prints
 * the execution order for comparison.
 */

typedef struct {
    int id;
    int priority;
} worker_arg_t;

void worker(void *arg) {
    worker_arg_t *w = (worker_arg_t*)arg;
    for (int i = 1; i <= 2; i++) {
        printf("  Thread %d (pri=%d): iteration %d\n", w->id, w->priority, i);
        uthread_yield();
    }
}

/*
 * We can't literally "run twice in the same process" because
 * uthread_exit() calls exit(0) when the queue empties.
 * Instead, we select the policy via a command-line argument:
 *   ./test_scheduler rr    -> round-robin
 *   ./test_scheduler prio  -> priority
 *
 * Run both from the shell to compare output.
 */
int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <rr|prio>\n", argv[0]);
        printf("  rr   = Round-Robin scheduling\n");
        printf("  prio = Priority scheduling (lower number = higher priority)\n");
        return 1;
    }

    uthread_init();

    if (argv[1][0] == 'p') {
        printf("=== Priority Scheduling ===\n");
        uthread_set_scheduler(priority_next);
    } else {
        printf("=== Round-Robin Scheduling ===\n");
        uthread_set_scheduler(round_robin_next);
    }

    /* 6 threads with mixed priorities */
    static worker_arg_t args[6] = {
        {1, 3},  /* low priority  */
        {2, 1},  /* high priority */
        {3, 2},  /* medium        */
        {4, 0},  /* highest       */
        {5, 2},  /* medium (tie with thread 3) */
        {6, 1},  /* high (tie with thread 2) */
    };

    for (int i = 0; i < 6; i++) {
        uthread_create_with_priority(worker, &args[i], args[i].priority);
    }

    /* Main thread exits, letting workers run */
    uthread_exit();

    return 0;
}
