#include "uthread.h"
#include <stdio.h>
#include <stdlib.h>

/*
 * Test: Guard Page Detection
 *
 * Creates a normal thread that runs fine, confirming guard pages
 * don't interfere with regular operation.
 *
 * For the actual stack overflow test, see test_guard_page_overflow.c
 * which uses fork() to safely test the SIGSEGV guard page handler.
 */

static void safe_worker(void *arg) {
    (void)arg;
    printf("  Safe thread running normally with guard pages active.\n");
    printf("  Stack allocation: mmap + mprotect guard page.\n");
}

static void multi_frame_worker(void *arg) {
    int depth = *(int*)arg;
    volatile char local[256];  /* use some stack space */
    local[0] = (char)depth;
    local[255] = (char)depth;
    if (depth > 0) {
        int next = depth - 1;
        multi_frame_worker(&next);
    } else {
        printf("  Deep recursion thread completed (256 bytes x frames).\n");
    }
}

int main(void) {
    printf("=== Test: Guard Page Stack Safety ===\n\n");

    uthread_init();

    printf("[Test 1] Normal thread with guard pages:\n");
    uthread_create(safe_worker, NULL);

    printf("[Test 2] Controlled deep recursion (should succeed):\n");
    int depth = 50;  /* 50 frames x 256 bytes = 12.8KB, well within 64KB */
    uthread_create(multi_frame_worker, &depth);

    uthread_exit();

    return 0;
}
