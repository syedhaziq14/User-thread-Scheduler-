#define _XOPEN_SOURCE 700
#include "uthread.h"
#include <ucontext.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <signal.h>
#include <sys/time.h>

/*
 * PREEMPTION AND SIGNAL BLOCKING:
 * 
 * To implement preemptive scheduling, we use a SIGALRM timer that fires
 * every TIME_QUANTUM_MS. The signal handler automatically calls uthread_yield()
 * to force a context switch, ensuring threads cannot monopolise the CPU.
 * 
 * Critical Safety Requirement:
 * While the scheduler itself is running (e.g., inside uthread_yield, uthread_create,
 * uthread_exit, or inside mutex/semaphore lock/unlock functions), we are actively
 * modifying shared scheduler state such as the ready queue array, wait queues,
 * and TCB state fields.
 * 
 * Race Condition Prevented:
 * If a SIGALRM fires mid-modification of the ready queue array (for instance, while
 * enqueue() is halfway through updating q_tail and q_size), the signal handler would
 * call uthread_yield() and re-enter the scheduler. This re-entrant call would then
 * attempt to modify the same partially-updated ready queue or pick a thread that is 
 * in an inconsistent state, leading to queue corruption, lost threads, or segmentation
 * faults.
 * 
 * To prevent this, we use sigprocmask(SIG_BLOCK, ...) to block SIGALRM before
 * entering any critical scheduler section, and unblock it when leaving or when
 * transferring control to user code.
 */

static sigset_t sigalrm_set;

static void block_signals(void) {
    sigprocmask(SIG_BLOCK, &sigalrm_set, NULL);
}

static void unblock_signals(void) {
    sigprocmask(SIG_UNBLOCK, &sigalrm_set, NULL);
}

static void alrm_handler(int signum) {
    (void)signum;
    uthread_yield();
}

#define MAX_THREADS UTHREAD_MAX_THREADS

/* ---------- Thread Control Block ---------- */

typedef enum {
    READY,
    RUNNING,
    BLOCKED,
    FINISHED
} uthread_state_t;

struct tcb {
    int thread_id;
    ucontext_t context;
    void *stack;
    uthread_state_t state;
    void (*fn)(void*);
    void *arg;
    int priority;
    int creation_order;
};

/* ---------- Global State ---------- */

static tcb_t threads[MAX_THREADS];
static int current_thread_id = -1;
static int creation_counter = 0;

/* ---------- Ready Queue ---------- */

static int ready_queue[MAX_THREADS];
static int q_head = 0;
static int q_tail = 0;
static int q_size = 0;

static void enqueue(int tid) {
    if (q_size < MAX_THREADS) {
        ready_queue[q_tail] = tid;
        q_tail = (q_tail + 1) % MAX_THREADS;
        q_size++;
    }
}

static int dequeue(void) {
    if (q_size > 0) {
        int tid = ready_queue[q_head];
        q_head = (q_head + 1) % MAX_THREADS;
        q_size--;
        return tid;
    }
    return -1;
}

/* ---------- Pluggable Scheduler ---------- */

static scheduler_fn current_scheduler = NULL;

tcb_t* round_robin_next(void) {
    int tid = dequeue();
    if (tid == -1) return NULL;
    return &threads[tid];
}

tcb_t* priority_next(void) {
    int best_tid = -1;
    int best_priority = __INT_MAX__;
    int best_creation = __INT_MAX__;

    for (int i = 0; i < MAX_THREADS; i++) {
        if (threads[i].state == READY) {
            if (threads[i].priority < best_priority ||
                (threads[i].priority == best_priority &&
                 threads[i].creation_order < best_creation)) {
                best_tid = i;
                best_priority = threads[i].priority;
                best_creation = threads[i].creation_order;
            }
        }
    }

    if (best_tid == -1) return NULL;

    int new_size = 0;
    for (int i = 0; i < q_size; i++) {
        int idx = (q_head + i) % MAX_THREADS;
        if (ready_queue[idx] != best_tid) {
            ready_queue[(q_head + new_size) % MAX_THREADS] = ready_queue[idx];
            new_size++;
        }
    }
    q_size = new_size;
    q_tail = (q_head + new_size) % MAX_THREADS;

    return &threads[best_tid];
}

static tcb_t* schedule_next(void) {
    if (current_scheduler) {
        return current_scheduler();
    }
    return round_robin_next();
}

/* ---------- Stack Cleanup ---------- */

static void free_finished_stacks(void) {
    for (int i = 0; i < MAX_THREADS; i++) {
        if (threads[i].state == FINISHED && threads[i].stack != NULL) {
            if (i != current_thread_id) {
                free(threads[i].stack);
                threads[i].stack = NULL;
            }
        }
    }
}

/* ---------- Thread Wrapper ---------- */

static void thread_wrapper(void) {
    /* Newly created threads start here. Unblock signals so they can be preempted. */
    unblock_signals();

    tcb_t *tcb = &threads[current_thread_id];
    tcb->fn(tcb->arg);
    uthread_exit();
}

/* ---------- Core API ---------- */

void uthread_init(void) {
    for (int i = 0; i < MAX_THREADS; i++) {
        threads[i].state = FINISHED;
        threads[i].stack = NULL;
        threads[i].priority = 0;
        threads[i].creation_order = 0;
    }

    q_head = 0;
    q_tail = 0;
    q_size = 0;
    creation_counter = 0;
    current_scheduler = NULL;

    current_thread_id = 0;
    tcb_t *main_tcb = &threads[current_thread_id];
    main_tcb->thread_id = current_thread_id;
    main_tcb->state = RUNNING;
    main_tcb->creation_order = creation_counter++;
    getcontext(&main_tcb->context);

    /* Setup preemption timer and signal mask */
    sigemptyset(&sigalrm_set);
    sigaddset(&sigalrm_set, SIGALRM);

    struct sigaction sa;
    sa.sa_handler = alrm_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &sa, NULL);

    struct itimerval it;
    it.it_value.tv_sec = 0;
    it.it_value.tv_usec = TIME_QUANTUM_MS * 1000;
    it.it_interval.tv_sec = 0;
    it.it_interval.tv_usec = TIME_QUANTUM_MS * 1000;
    setitimer(ITIMER_REAL, &it, NULL);
}

int uthread_create_with_priority(void (*fn)(void*), void *arg, int priority) {
    block_signals();
    free_finished_stacks();

    int tid = -1;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (threads[i].state == FINISHED && threads[i].stack == NULL) {
            tid = i;
            break;
        }
    }

    if (tid == -1) {
        unblock_signals();
        return -1;
    }

    tcb_t *tcb = &threads[tid];
    tcb->thread_id = tid;
    tcb->state = READY;
    tcb->fn = fn;
    tcb->arg = arg;
    tcb->priority = priority;
    tcb->creation_order = creation_counter++;

    tcb->stack = malloc(UTHREAD_STACK_SIZE);
    if (!tcb->stack) {
        tcb->state = FINISHED;
        unblock_signals();
        return -1;
    }

    /* getcontext saves the current signal mask, which currently has SIGALRM blocked. 
       This is fine because thread_wrapper explicitly unblocks it. */
    getcontext(&tcb->context);
    tcb->context.uc_stack.ss_sp = tcb->stack;
    tcb->context.uc_stack.ss_size = UTHREAD_STACK_SIZE;
    tcb->context.uc_link = NULL;

    makecontext(&tcb->context, thread_wrapper, 0);

    enqueue(tid);

    unblock_signals();
    return tid;
}

int uthread_create(void (*fn)(void*), void *arg) {
    return uthread_create_with_priority(fn, arg, 0);
}

void uthread_yield(void) {
    block_signals();
    free_finished_stacks();

    int prev_tid = current_thread_id;
    if (threads[prev_tid].state == RUNNING) {
        threads[prev_tid].state = READY;
        enqueue(prev_tid);
    }

    tcb_t *next = schedule_next();
    if (next == NULL) {
        if (threads[prev_tid].state == READY) {
            threads[prev_tid].state = RUNNING;
            unblock_signals();
            return;
        } else {
            exit(0);
        }
    }

    current_thread_id = next->thread_id;
    threads[current_thread_id].state = RUNNING;

    /* Context switch. When this thread is scheduled again, it resumes execution 
       right after this swapcontext call and immediately unblocks signals. */
    swapcontext(&threads[prev_tid].context, &threads[current_thread_id].context);

    unblock_signals();
}

void uthread_exit(void) {
    block_signals();
    threads[current_thread_id].state = FINISHED;

    tcb_t *next = schedule_next();
    if (next == NULL) {
        free_finished_stacks();
        exit(0);
    }

    int prev_tid = current_thread_id;
    current_thread_id = next->thread_id;
    threads[current_thread_id].state = RUNNING;

    /* We never return from this swapcontext */
    swapcontext(&threads[prev_tid].context, &threads[current_thread_id].context);
}

void uthread_set_scheduler(scheduler_fn policy) {
    current_scheduler = policy;
}

/* ---------- Mutex API ---------- */

void uthread_mutex_init(uthread_mutex_t *m) {
    m->locked = 0;
    m->wait_count = 0;
}

void uthread_mutex_lock(uthread_mutex_t *m) {
    block_signals();
    if (!m->locked) {
        m->locked = 1;
        unblock_signals();
        return;
    }

    threads[current_thread_id].state = BLOCKED;
    m->wait_queue[m->wait_count++] = current_thread_id;
    
    /* Yield to another thread. 
       When this thread resumes, signals will be unblocked inside uthread_yield. */
    uthread_yield();
}

void uthread_mutex_unlock(uthread_mutex_t *m) {
    block_signals();
    if (m->wait_count > 0) {
        int tid = m->wait_queue[0];
        for (int i = 0; i < m->wait_count - 1; i++) {
            m->wait_queue[i] = m->wait_queue[i + 1];
        }
        m->wait_count--;
        threads[tid].state = READY;
        enqueue(tid);
    } else {
        m->locked = 0;
    }
    unblock_signals();
}

/* ---------- Semaphore API ---------- */

void uthread_sem_init(uthread_sem_t *s, int initial_count) {
    s->count = initial_count;
    s->wait_count = 0;
}

void uthread_sem_wait(uthread_sem_t *s) {
    block_signals();
    if (s->count > 0) {
        s->count--;
        unblock_signals();
        return;
    }

    threads[current_thread_id].state = BLOCKED;
    s->wait_queue[s->wait_count++] = current_thread_id;
    
    /* Unblocking handled inside uthread_yield */
    uthread_yield();
}

void uthread_sem_post(uthread_sem_t *s) {
    block_signals();
    s->count++;
    if (s->wait_count > 0) {
        s->count--;
        int tid = s->wait_queue[0];
        for (int i = 0; i < s->wait_count - 1; i++) {
            s->wait_queue[i] = s->wait_queue[i + 1];
        }
        s->wait_count--;
        threads[tid].state = READY;
        enqueue(tid);
    }
    unblock_signals();
}

/* ---------- Channel API ---------- */

void uthread_channel_init(uthread_channel_t *ch, int capacity) {
    if (capacity > UTHREAD_CHANNEL_MAX_CAPACITY) {
        capacity = UTHREAD_CHANNEL_MAX_CAPACITY;
    }
    ch->capacity = capacity;
    ch->head = 0;
    ch->tail = 0;
    
    uthread_sem_init(&ch->space_available, capacity);
    uthread_sem_init(&ch->messages_available, 0);
    uthread_mutex_init(&ch->mutex);
}

void uthread_channel_send(uthread_channel_t *ch, void *msg) {
    /* The underlying primitives handle signal blocking, 
       so we don't need explicit block_signals() here. */
    uthread_sem_wait(&ch->space_available);
    uthread_mutex_lock(&ch->mutex);
    
    ch->buffer[ch->tail] = msg;
    ch->tail = (ch->tail + 1) % ch->capacity;
    
    uthread_mutex_unlock(&ch->mutex);
    uthread_sem_post(&ch->messages_available);
}

void* uthread_channel_recv(uthread_channel_t *ch) {
    uthread_sem_wait(&ch->messages_available);
    uthread_mutex_lock(&ch->mutex);
    
    void *msg = ch->buffer[ch->head];
    ch->head = (ch->head + 1) % ch->capacity;
    
    uthread_mutex_unlock(&ch->mutex);
    uthread_sem_post(&ch->space_available);
    
    return msg;
}
