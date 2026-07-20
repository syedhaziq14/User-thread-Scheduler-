#include "uthread.h"
#include <stdio.h>
#include <stdlib.h>

#define ITERATIONS 500000

/* 
 * Classic Peterson's Solution
 * No atomics, no memory barriers used on purpose for academic demonstration.
 */
int flag[2] = {0, 0};
int turn = 0;
int counter = 0;
int finished = 0;

void lock(int self) {
    int other = 1 - self;
    flag[self] = 1;
    turn = other;
    
    /* 
     * COMPILER REORDERING / OUT-OF-ORDER EXECUTION VULNERABILITY:
     * A real modern compiler (like GCC -O2 or -O3) or a modern CPU (like x86/ARM) 
     * can reorder the writes to flag[] and turn, or reorder the reads 
     * in the while loop before the writes, because there are no memory 
     * barriers or atomics used here. 
     * If reordering happens, both threads can see the old values and 
     * simultaneously enter the critical section, causing a race condition.
     */
    while (flag[other] == 1 && turn == other) {
        uthread_yield(); // Prevent hard hang in user-space threads
    }
}

void unlock(int self) {
    flag[self] = 0;
}

void worker(void *arg) {
    int id = *(int*)arg;
    for (int i = 0; i < ITERATIONS; i++) {
        lock(id);
        
        /* Critical Section */
        int temp = counter;
        if (i % 50000 == 0) uthread_yield(); // Increase chance of preemption race
        counter = temp + 1;
        
        unlock(id);
    }
    
    /* Wait for both to finish before printing result */
    lock(id);
    finished++;
    if (finished == 2) {
        printf("Final counter value: %d (Expected: %d)\n", counter, ITERATIONS * 2);
        if (counter != ITERATIONS * 2) {
            printf("FAILED: Mutual exclusion violated due to memory reordering!\n");
        } else {
            printf("PASS: Mutual exclusion preserved.\n");
        }
        exit(0);
    }
    unlock(id);
}

int main(void) {
    uthread_init();
    
    static int id0 = 0;
    static int id1 = 1;
    
    uthread_create(worker, &id0);
    uthread_create(worker, &id1);
    
    uthread_exit();
    return 0;
}
