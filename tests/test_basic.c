#include "uthread.h"
#include <stdio.h>

int main(void) {
    printf("Initializing uthread...\n");
    uthread_init();
    printf("Done.\n");
    return 0;
}
