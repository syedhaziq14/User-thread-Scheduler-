#include "uthread.h"
#include <stdio.h>
#include <stdlib.h>

/*
 * Test: Preemptive Scheduling
 *
 * Three threads:
 * - Thread 1: Infinite CPU-bound loop, never yields voluntarily.
 * - Thread 2 & 3: Yields voluntarily and prints periodically.
 *
 * Goal: Prove that Thread 1 gets interrupted and all threads make progress.
 */

static volatile int thread1_progress = 0;
static int yielding_finished = 0;

void infinite_loop_thread(void *arg) {
    (void)arg;
    while (1) {
        /* Do some dummy work to consume CPU cycles */
        volatile int x = 0;
        for (int i = 0; i < 5000000; i++) {
            x++;
        }
        thread1_progress++;
        /* Notice: NO uthread_yield() here!
           If preemption doesn't work, this thread will run forever
           and the other threads will starve. */
    }
}

void yielding_thread(void *arg) {
    int id = *(int*)arg;
    for (int i = 1; i <= 3; i++) {
        printf("  Thread %d (yielding): iteration %d, infinite thread progress = %d\n", 
               id, i, thread1_progress);
        
        /* Do a little work */
        volatile int x = 0;
        for (int j = 0; j < 10000000; j++) {
            x++;
        }
        
        uthread_yield();
    }
    
    yielding_finished++;
    if (yielding_finished == 2) {
        printf("\nBoth yielding threads finished!\n");
        printf("Infinite thread reached progress %d.\n", thread1_progress);
        printf("Preemption works. Exiting cleanly.\n");
        exit(0);
    }
}

int main(void) {
    printf("--- Preemptive Scheduling Test Start ---\n");
    uthread_init();

    int id2 = 2;
    int id3 = 3;

    /* Create the CPU-bound thread that never yields */
    uthread_create(infinite_loop_thread, NULL);
    
    /* Create two well-behaved threads that yield */
    uthread_create(yielding_thread, &id2);
    uthread_create(yielding_thread, &id3);

    uthread_exit();
    return 0;
}
