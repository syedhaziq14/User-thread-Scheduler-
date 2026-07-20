#include "uthread.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * Test: Guard Page — Overflow Child
 *
 * This test forks a child process that deliberately overflows its stack.
 * The parent verifies the child is killed by the guard page SIGSEGV handler
 * (exit code 139).
 */

static void stack_overflow_recursive(int depth) {
    volatile char buffer[4096];
    buffer[0] = (char)depth;
    buffer[4095] = (char)depth;
    stack_overflow_recursive(depth + 1);
}

static void overflow_worker(void *arg) {
    (void)arg;
    stack_overflow_recursive(0);
}

int main(void) {
    printf("=== Test: Guard Page Overflow (Fork Test) ===\n\n");

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: this will trigger guard page SIGSEGV */
        uthread_init();
        uthread_create(overflow_worker, NULL);
        uthread_exit();
        _exit(0);  /* should not reach */
    }

    /* Parent: wait for child */
    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 139) {
        printf("PASS: Child exited with code 139 (stack overflow detected!)\n");
    } else if (WIFSIGNALED(status) && WTERMSIG(status) == 11) {
        printf("PASS: Child killed by SIGSEGV (guard page worked)\n");
    } else {
        printf("Status: exited=%d code=%d signaled=%d sig=%d\n",
               WIFEXITED(status), WEXITSTATUS(status),
               WIFSIGNALED(status), WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        printf("FAIL: Unexpected child exit status\n");
        return 1;
    }

    return 0;
}
