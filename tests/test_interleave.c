#include "uthread.h"
#include <stdio.h>

void worker(void *arg) {
    int id = *(int*)arg;
    for (int i = 1; i <= 3; i++) {
        printf("Thread %d: iteration %d\n", id, i);
        uthread_yield();
    }
    // Thread will naturally return here and should be marked FINISHED automatically
}

int main(void) {
    printf("Starting uthread test...\n");
    uthread_init();
    
    int ids[4] = {1, 2, 3, 4};
    for (int i = 0; i < 4; i++) {
        uthread_create(worker, &ids[i]);
    }
    
    // The main thread calls uthread_exit to let the workers run.
    // Since main exits, when all workers finish the ready queue will be empty and the program will exit cleanly.
    uthread_exit();
    
    return 0;
}
