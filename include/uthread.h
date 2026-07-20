#ifndef UTHREAD_H
#define UTHREAD_H

#include <stddef.h>
#include <sys/types.h>

#ifndef UTHREAD_STACK_SIZE
#define UTHREAD_STACK_SIZE (64 * 1024)
#endif

#ifndef TIME_QUANTUM_MS
#define TIME_QUANTUM_MS 10
#endif

/* ---------- Thread States ---------- */

typedef enum {
    UTHREAD_READY,
    UTHREAD_RUNNING,
    UTHREAD_BLOCKED,
    UTHREAD_IO_BLOCKED,
    UTHREAD_FINISHED
} uthread_state_t;

/* ---------- Forward Declarations ---------- */

typedef struct tcb tcb_t;

/* Scheduler policy function pointer type */
typedef tcb_t* (*scheduler_fn)(void);

/* ---------- TCB Definition ---------- */
/*
 * The TCB is now dynamically allocated. It contains intrusive linked-list
 * pointers for ready queues, wait queues, and global thread tracking.
 */
struct tcb {
    int              thread_id;
    uthread_state_t  state;
    void            *stack;          /* mmap'd stack region (including guard page) */
    size_t           stack_alloc;    /* total mmap'd size (guard page + usable stack) */
    void           (*fn)(void*);
    void            *arg;
    int              priority;
    int              creation_order;

    /* Intrusive linked-list pointers */
    tcb_t           *next;           /* next in ready queue or wait queue */
    tcb_t           *global_next;    /* next in global thread list (for GC) */
    tcb_t           *global_prev;    /* prev in global thread list (for GC) */

    /* Async I/O fields (Phase 12) */
    int              io_fd;          /* fd being waited on (-1 if none) */
    int              io_events;      /* epoll events to wait for */
    ssize_t          io_result;      /* result of I/O operation */
    unsigned long long sleep_until_ns; /* monotonic ns wakeup time (0 = not sleeping) */

    /* M:N fields (Phase 14) */
    int              vp_id;          /* which virtual processor is running this */

    /* ucontext must be last — it's large */
    /* We include ucontext_t via the source file, not here, to avoid pulling in
       platform headers. We store it as an opaque region. Actually, we do need
       the full definition for makecontext/swapcontext, so we include it. */
};

/* ---------- Mutex ---------- */

typedef struct {
    int    locked;
    tcb_t *wait_head;
    tcb_t *wait_tail;
} uthread_mutex_t;

/* ---------- Semaphore ---------- */

typedef struct {
    int    count;
    tcb_t *wait_head;
    tcb_t *wait_tail;
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
int  uthread_self(void);

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

/* ---------- Async I/O API (Phase 12) ---------- */

void    uthread_sleep(unsigned int ms);
ssize_t uthread_read(int fd, void *buf, size_t count);
ssize_t uthread_write(int fd, const void *buf, size_t count);

/* ---------- Tracing API (Phase 13) ---------- */

void uthread_trace_enable(void);
void uthread_trace_disable(void);
void uthread_dump_trace(const char *filename);

/* ---------- M:N API (Phase 14) ---------- */

void uthread_init_mn(int num_vps);
void uthread_mn_run(void);

#endif /* UTHREAD_H */
