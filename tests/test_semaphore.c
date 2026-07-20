#include "uthread.h"
#include <stdio.h>
#include <string.h>

/*
 * Classic bounded-buffer producer-consumer using semaphores.
 *
 * - Circular buffer of size 5
 * - 2 producers generate items 0-19 total between them (10 each)
 * - 2 consumers consume all 20 items and print them
 * - Semaphores: empty_slots (init 5), filled_slots (init 0)
 * - Mutex protects buffer index operations
 *
 * Correctness: all 20 items consumed exactly once, no deadlock.
 */

#define BUF_SIZE      5
#define TOTAL_ITEMS  20
#define ITEMS_PER_PRODUCER (TOTAL_ITEMS / 2)

/* Circular buffer */
static int buffer[BUF_SIZE];
static int buf_in  = 0;   /* next write position */
static int buf_out = 0;   /* next read position  */

/* Synchronisation primitives */
static uthread_sem_t   empty_slots;
static uthread_sem_t   filled_slots;
static uthread_mutex_t buf_mutex;

/* Shared counter for item production */
static int next_item = 0;
static uthread_mutex_t item_mutex;

/* Tracking consumption for verification */
static int consumed[TOTAL_ITEMS];
static int consume_count = 0;
static uthread_mutex_t consume_mutex;

void producer(void *arg) {
    int id = *(int*)arg;

    for (int i = 0; i < ITEMS_PER_PRODUCER; i++) {
        /* Get the next globally-unique item to produce */
        uthread_mutex_lock(&item_mutex);
        int item = next_item++;
        uthread_mutex_unlock(&item_mutex);

        /* Wait for an empty slot */
        uthread_sem_wait(&empty_slots);

        /* Place item in buffer (protected by mutex) */
        uthread_mutex_lock(&buf_mutex);
        buffer[buf_in] = item;
        buf_in = (buf_in + 1) % BUF_SIZE;
        uthread_mutex_unlock(&buf_mutex);

        printf("  Producer %d: produced item %2d\n", id, item);

        /* Signal that a slot is now filled */
        uthread_sem_post(&filled_slots);

        uthread_yield();
    }
}

void consumer(void *arg) {
    int id = *(int*)arg;

    while (1) {
        /* Check if all items have been consumed */
        uthread_mutex_lock(&consume_mutex);
        if (consume_count >= TOTAL_ITEMS) {
            uthread_mutex_unlock(&consume_mutex);
            break;
        }
        consume_count++;
        uthread_mutex_unlock(&consume_mutex);

        /* Wait for a filled slot */
        uthread_sem_wait(&filled_slots);

        /* Take item from buffer (protected by mutex) */
        uthread_mutex_lock(&buf_mutex);
        int item = buffer[buf_out];
        buf_out = (buf_out + 1) % BUF_SIZE;
        uthread_mutex_unlock(&buf_mutex);

        consumed[item] = 1;

        printf("  Consumer %d: consumed item %2d\n", id, item);

        /* Signal that a slot is now empty */
        uthread_sem_post(&empty_slots);

        uthread_yield();
    }
}

int main(void) {
    uthread_init();

    /* Initialise primitives */
    uthread_sem_init(&empty_slots, BUF_SIZE);
    uthread_sem_init(&filled_slots, 0);
    uthread_mutex_init(&buf_mutex);
    uthread_mutex_init(&item_mutex);
    uthread_mutex_init(&consume_mutex);

    /* Reset tracking state */
    memset(consumed, 0, sizeof(consumed));
    consume_count = 0;
    next_item = 0;
    buf_in = 0;
    buf_out = 0;

    printf("--- Producer-Consumer Start ---\n");

    static int pids[2] = {1, 2};
    static int cids[2] = {1, 2};

    uthread_create(producer, &pids[0]);
    uthread_create(producer, &pids[1]);
    uthread_create(consumer, &cids[0]);
    uthread_create(consumer, &cids[1]);

    /* Let all threads run to completion */
    uthread_exit();

    return 0;
}
