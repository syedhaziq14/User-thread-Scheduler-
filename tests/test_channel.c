#include "uthread.h"
#include <stdio.h>
#include <stdlib.h>

#define NUM_ITEMS 10

uthread_channel_t channel_a;
uthread_channel_t channel_b;

void stage1(void *arg) {
    (void)arg;
    for (int i = 1; i <= NUM_ITEMS; i++) {
        int *msg = malloc(sizeof(int));
        *msg = i;
        printf("Stage 1: sending %d to Channel A\n", *msg);
        uthread_channel_send(&channel_a, msg);
    }
}

void stage2(void *arg) {
    (void)arg;
    for (int i = 0; i < NUM_ITEMS; i++) {
        int *msg = (int *)uthread_channel_recv(&channel_a);
        int original = *msg;
        *msg = original * 2;
        printf("  Stage 2: received %d from A, doubled to %d, sending to Channel B\n", original, *msg);
        uthread_channel_send(&channel_b, msg);
    }
}

void stage3(void *arg) {
    (void)arg;
    for (int i = 0; i < NUM_ITEMS; i++) {
        int *msg = (int *)uthread_channel_recv(&channel_b);
        printf("    Stage 3: received final result %d from Channel B\n", *msg);
        free(msg);
    }
    printf("\nPipeline complete! All %d items successfully processed.\n", NUM_ITEMS);
}

int main(void) {
    printf("--- Concurrent Channel Pipeline Test Start ---\n");
    uthread_init();
    
    /* Small capacities force the pipeline stages to block and interleave */
    uthread_channel_init(&channel_a, 3);
    uthread_channel_init(&channel_b, 3);
    
    uthread_create(stage1, NULL);
    uthread_create(stage2, NULL);
    uthread_create(stage3, NULL);
    
    uthread_exit();
    return 0;
}
