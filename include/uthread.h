#ifndef UTHREAD_H
#define UTHREAD_H

#ifndef UTHREAD_STACK_SIZE
#define UTHREAD_STACK_SIZE (64 * 1024)
#endif

#ifndef TIME_QUANTUM_MS
#define TIME_QUANTUM_MS 10
#endif

#define UTHREAD_MAX_THREADS 1024

/* Forward declaration of the TCB for the scheduler function pointer */
typedef struct tcb tcb_t;

/* Scheduler policy function pointer type */
typedef tcb_t* (*scheduler_fn)(void);

/* ---------- Mutex ---------- */

typedef struct {
    int locked;
    int wait_queue[UTHREAD_MAX_THREADS];
    int wait_count;
} uthread_mutex_t;

/* ---------- Semaphore ---------- */

typedef struct {
    int count;
    int wait_queue[UTHREAD_MAX_THREADS];
    int wait_count;
} uthread_sem_t;

/* ---------- Channel ---------- */

#ifndef UTHREAD_CHANNEL_MAX_CAPACITY
#define UTHREAD_CHANNEL_MAX_CAPACITY 64
#endif

typedef struct {
    void *buffer[UTHREAD_CHANNEL_MAX_CAPACITY];
    int head;
    int tail;
    int capacity;
    uthread_sem_t space_available;
    uthread_sem_t messages_available;
    uthread_mutex_t mutex;
} uthread_channel_t;

/* ---------- Core API ---------- */

void uthread_init(void);
int  uthread_create(void (*fn)(void*), void *arg);
int  uthread_create_with_priority(void (*fn)(void*), void *arg, int priority);
void uthread_yield(void);
void uthread_exit(void);

/* ---------- Scheduler Policy API ---------- */

void   uthread_set_scheduler(scheduler_fn policy);
tcb_t* round_robin_next(void);
tcb_t* priority_next(void);

/* ---------- Mutex API ---------- */

void uthread_mutex_init(uthread_mutex_t *m);
void uthread_mutex_lock(uthread_mutex_t *m);
void uthread_mutex_unlock(uthread_mutex_t *m);

/* ---------- Semaphore API ---------- */

void uthread_sem_init(uthread_sem_t *s, int initial_count);
void uthread_sem_wait(uthread_sem_t *s);
void uthread_sem_post(uthread_sem_t *s);

/* ---------- Channel API ---------- */

void  uthread_channel_init(uthread_channel_t *ch, int capacity);
void  uthread_channel_send(uthread_channel_t *ch, void *msg);
void* uthread_channel_recv(uthread_channel_t *ch);

#endif /* UTHREAD_H */
