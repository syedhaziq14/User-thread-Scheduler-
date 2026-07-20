#define _GNU_SOURCE
#include "uthread.h"
#include <ucontext.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sched.h>

/* =========================================================================
 * CONFIGURATION CONSTANTS
 * ========================================================================= */

#define GUARD_PAGE_SIZE     4096
#define TRACE_BUFFER_SIZE   (1 << 20)   /* 1M trace events */
#define MAX_EPOLL_EVENTS    64
#define MAX_VIRTUAL_PROCS   64

/* =========================================================================
 * INTERNAL TCB EXTENSION
 *
 * The public header exposes tcb_t without ucontext_t to keep the API clean.
 * Internally, we wrap tcb_t in tcb_internal_t which carries the ucontext.
 * ========================================================================= */

typedef struct tcb_internal {
    tcb_t        pub;         /* public TCB — MUST be first member for casting */
    ucontext_t   context;
} tcb_internal_t;

#define TCB_INTERNAL(tcb) ((tcb_internal_t*)(tcb))
#define TCB_CONTEXT(tcb)  (&TCB_INTERNAL(tcb)->context)

/* =========================================================================
 * FORWARD DECLARATIONS (needed for cross-section references)
 * ========================================================================= */

static void mn_yield(void);
static void mn_exit(void);
static void check_io_events(void);
static void check_sleep_list(void);

/* =========================================================================
 * PREEMPTION AND SIGNAL BLOCKING
 *
 * In single-core (1:N) mode, we prevent race conditions by blocking SIGALRM
 * during critical scheduler sections. In M:N mode (Phase 14), signals are
 * permanently blocked per-VP and we use spinlocks instead.
 * ========================================================================= */

static sigset_t sigalrm_set;
static volatile int mn_mode = 0;  /* 0 = single-core, 1 = M:N mode */

static void block_signals(void) {
    if (!mn_mode)
        sigprocmask(SIG_BLOCK, &sigalrm_set, NULL);
}

static void unblock_signals(void) {
    if (!mn_mode)
        sigprocmask(SIG_UNBLOCK, &sigalrm_set, NULL);
}

static void alrm_handler(int signum) {
    (void)signum;
    uthread_yield();
}

/* =========================================================================
 * SPINLOCK (for M:N cross-core safety)
 * ========================================================================= */

typedef struct {
    atomic_int lock;
} spinlock_t;

static inline void spinlock_init(spinlock_t *sl) {
    atomic_store(&sl->lock, 0);
}

static inline void spinlock_acquire(spinlock_t *sl) {
    int spin_count = 0;
    while (atomic_exchange(&sl->lock, 1) != 0) {
        if (spin_count < 100) {
#if defined(__x86_64__) || defined(__i386__)
            __asm__ __volatile__("pause");
#endif
            spin_count++;
        } else {
            sched_yield(); /* Yield the underlying OS thread if spinning too long */
        }
    }
}

static inline void spinlock_release(spinlock_t *sl) {
    atomic_store(&sl->lock, 0);
}

/* =========================================================================
 * TRACING (Phase 13)
 *
 * Low-overhead in-memory event recorder. Records scheduler transitions
 * with nanosecond timestamps. Dumps to Chrome Trace Event Format JSON.
 * ========================================================================= */

typedef enum {
    TRACE_CREATE,
    TRACE_READY,
    TRACE_RUN,
    TRACE_YIELD,
    TRACE_PREEMPT,
    TRACE_BLOCK,
    TRACE_UNBLOCK,
    TRACE_IO_BLOCK,
    TRACE_IO_UNBLOCK,
    TRACE_SLEEP,
    TRACE_WAKEUP,
    TRACE_EXIT,
    TRACE_STEAL
} trace_event_type_t;

typedef struct {
    int                  tid;
    trace_event_type_t   type;
    uint64_t             timestamp_ns;
    int                  core_id;
} trace_event_t;

static trace_event_t *trace_buffer = NULL;
static atomic_int      trace_count = 0;
static int             trace_enabled = 0;

static uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void trace_emit(int tid, trace_event_type_t type, int core_id) {
    if (!trace_enabled || !trace_buffer) return;
    int idx = atomic_fetch_add(&trace_count, 1);
    if (idx >= TRACE_BUFFER_SIZE) {
        atomic_fetch_sub(&trace_count, 1);
        return;
    }
    trace_buffer[idx].tid = tid;
    trace_buffer[idx].type = type;
    trace_buffer[idx].timestamp_ns = get_time_ns();
    trace_buffer[idx].core_id = core_id;
}

static const char* trace_event_name(trace_event_type_t t) {
    switch (t) {
        case TRACE_CREATE:     return "CREATE";
        case TRACE_READY:      return "READY";
        case TRACE_RUN:        return "RUN";
        case TRACE_YIELD:      return "YIELD";
        case TRACE_PREEMPT:    return "PREEMPT";
        case TRACE_BLOCK:      return "BLOCK";
        case TRACE_UNBLOCK:    return "UNBLOCK";
        case TRACE_IO_BLOCK:   return "IO_BLOCK";
        case TRACE_IO_UNBLOCK: return "IO_UNBLOCK";
        case TRACE_SLEEP:      return "SLEEP";
        case TRACE_WAKEUP:     return "WAKEUP";
        case TRACE_EXIT:       return "EXIT";
        case TRACE_STEAL:      return "STEAL";
    }
    return "UNKNOWN";
}

static const char* trace_event_phase(trace_event_type_t t) {
    switch (t) {
        case TRACE_RUN:        return "B";  /* begin duration */
        case TRACE_YIELD:
        case TRACE_PREEMPT:
        case TRACE_BLOCK:
        case TRACE_IO_BLOCK:
        case TRACE_SLEEP:
        case TRACE_EXIT:       return "E";  /* end duration */
        default:               return "i";  /* instant */
    }
}

/* =========================================================================
 * GLOBAL STATE
 * ========================================================================= */

/* Global thread list (doubly-linked for O(1) removal during GC) */
static tcb_t     *global_thread_list = NULL;
static spinlock_t global_list_lock;

/* Thread ID counter (atomic for M:N safety) */
static atomic_int next_thread_id = 0;

/* Single-core scheduler state */
static __thread tcb_t *current_thread = NULL;
static atomic_int creation_counter = 0;

/* Ready queue (intrusive singly-linked list) */
static tcb_t     *ready_head = NULL;
static tcb_t     *ready_tail = NULL;
static int        ready_count = 0;
static spinlock_t ready_lock;

/* Pluggable scheduler */
static scheduler_fn current_scheduler = NULL;

/* Epoll fd (Phase 12) */
static int epoll_fd = -1;

/* Sleep list (Phase 12) */
static tcb_t     *sleep_list_head = NULL;
static spinlock_t sleep_list_lock;

/* =========================================================================
 * READY QUEUE OPERATIONS (Intrusive Linked List)
 * ========================================================================= */

static void enqueue(tcb_t *tcb) {
    tcb->next = NULL;
    if (ready_tail) {
        ready_tail->next = tcb;
    } else {
        ready_head = tcb;
    }
    ready_tail = tcb;
    ready_count++;
}

static tcb_t* dequeue(void) {
    if (!ready_head) return NULL;
    tcb_t *tcb = ready_head;
    ready_head = tcb->next;
    if (!ready_head) ready_tail = NULL;
    tcb->next = NULL;
    ready_count--;
    return tcb;
}

static void remove_from_ready_queue(tcb_t *target) {
    tcb_t *prev = NULL;
    tcb_t *cur = ready_head;
    while (cur) {
        if (cur == target) {
            if (prev) prev->next = cur->next;
            else      ready_head = cur->next;
            if (cur == ready_tail) ready_tail = prev;
            cur->next = NULL;
            ready_count--;
            return;
        }
        prev = cur;
        cur = cur->next;
    }
}

/* =========================================================================
 * GLOBAL THREAD LIST MANAGEMENT
 * ========================================================================= */

static void global_list_add(tcb_t *tcb) {
    tcb->global_prev = NULL;
    tcb->global_next = global_thread_list;
    if (global_thread_list)
        global_thread_list->global_prev = tcb;
    global_thread_list = tcb;
}

static void global_list_remove(tcb_t *tcb) {
    if (tcb->global_prev)
        tcb->global_prev->global_next = tcb->global_next;
    else
        global_thread_list = tcb->global_next;
    if (tcb->global_next)
        tcb->global_next->global_prev = tcb->global_prev;
    tcb->global_prev = NULL;
    tcb->global_next = NULL;
}

/* =========================================================================
 * GUARD PAGE STACK ALLOCATION (Phase 11)
 *
 * Each thread stack is allocated via mmap with an extra guard page at the
 * bottom. The guard page is mprotect'd to PROT_NONE, so any stack overflow
 * triggers a SIGSEGV that we catch and report instead of silent corruption.
 * ========================================================================= */

static void* alloc_guarded_stack(size_t *out_alloc_size) {
    size_t alloc_size = UTHREAD_STACK_SIZE + GUARD_PAGE_SIZE;
    void *region = mmap(NULL, alloc_size,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED) return NULL;

    if (mprotect(region, GUARD_PAGE_SIZE, PROT_NONE) != 0) {
        munmap(region, alloc_size);
        return NULL;
    }

    *out_alloc_size = alloc_size;
    return region;
}

static void free_guarded_stack(void *region, size_t alloc_size) {
    if (region && alloc_size > 0)
        munmap(region, alloc_size);
}

static void sigsegv_handler(int signum, siginfo_t *si, void *ctx) {
    (void)signum;
    (void)ctx;
    void *fault_addr = si->si_addr;

    tcb_t *tcb = global_thread_list;
    while (tcb) {
        if (tcb->stack) {
            void *guard_lo = tcb->stack;
            void *guard_hi = (char*)tcb->stack + GUARD_PAGE_SIZE;
            if (fault_addr >= guard_lo && fault_addr < guard_hi) {
                fprintf(stderr,
                    "\n*** STACK OVERFLOW DETECTED ***\n"
                    "Thread %d overflowed its stack!\n"
                    "Fault address: %p (guard page: %p - %p)\n"
                    "Stack size: %d bytes. Consider increasing UTHREAD_STACK_SIZE.\n\n",
                    tcb->thread_id, fault_addr, guard_lo, guard_hi,
                    UTHREAD_STACK_SIZE);
                _exit(139);
            }
        }
        tcb = tcb->global_next;
    }

    /* Not a guard page fault — re-raise */
    struct sigaction sa;
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGSEGV, &sa, NULL);
    raise(SIGSEGV);
}

/* =========================================================================
 * GARBAGE COLLECTION
 * ========================================================================= */

static void gc_finished_threads(void) {
    tcb_t *tcb = global_thread_list;
    while (tcb) {
        tcb_t *next = tcb->global_next;
        if (tcb->state == UTHREAD_FINISHED && tcb != current_thread) {
            global_list_remove(tcb);
            free_guarded_stack(tcb->stack, tcb->stack_alloc);
            free(tcb);
        }
        tcb = next;
    }
}

/* =========================================================================
 * TCB ALLOCATION
 * ========================================================================= */

static tcb_t* alloc_tcb(void) {
    tcb_internal_t *ti = calloc(1, sizeof(tcb_internal_t));
    if (!ti) return NULL;
    ti->pub.io_fd = -1;
    return &ti->pub;
}

/* =========================================================================
 * PLUGGABLE SCHEDULER
 * ========================================================================= */

tcb_t* round_robin_next(void) {
    return dequeue();
}

tcb_t* priority_next(void) {
    tcb_t *best = NULL;
    tcb_t *cur = ready_head;
    while (cur) {
        if (cur->state == UTHREAD_READY) {
            if (!best ||
                cur->priority < best->priority ||
                (cur->priority == best->priority &&
                 cur->creation_order < best->creation_order)) {
                best = cur;
            }
        }
        cur = cur->next;
    }
    if (best) remove_from_ready_queue(best);
    return best;
}

static tcb_t* schedule_next(void) {
    if (current_scheduler) return current_scheduler();
    return round_robin_next();
}

/* =========================================================================
 * ASYNC I/O HELPERS (Phase 12)
 * ========================================================================= */

static void check_io_events(void) {
    if (epoll_fd < 0) return;

    struct epoll_event events[MAX_EPOLL_EVENTS];
    int n = epoll_wait(epoll_fd, events, MAX_EPOLL_EVENTS, 0);
    for (int i = 0; i < n; i++) {
        tcb_t *tcb = (tcb_t*)events[i].data.ptr;
        if (tcb && tcb->state == UTHREAD_IO_BLOCKED) {
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, tcb->io_fd, NULL);
            tcb->state = UTHREAD_READY;
            trace_emit(tcb->thread_id, TRACE_IO_UNBLOCK, 0);
            enqueue(tcb);
        }
    }
}

static void check_sleep_list(void) {
    uint64_t now = get_time_ns();
    spinlock_acquire(&sleep_list_lock);

    tcb_t *prev = NULL;
    tcb_t *cur = sleep_list_head;
    while (cur) {
        tcb_t *nx = cur->next;
        if (now >= cur->sleep_until_ns) {
            if (prev) prev->next = nx;
            else      sleep_list_head = nx;
            cur->sleep_until_ns = 0;
            cur->state = UTHREAD_READY;
            cur->next = NULL;
            trace_emit(cur->thread_id, TRACE_WAKEUP, 0);
            enqueue(cur);
        } else {
            prev = cur;
        }
        cur = nx;
    }

    spinlock_release(&sleep_list_lock);
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* =========================================================================
 * M:N VIRTUAL PROCESSOR (Phase 14)
 *
 * Each VP is a native pthread with its own local run queue.
 * Work stealing: when a VP's queue is empty, it steals half from a victim.
 * ========================================================================= */

typedef struct {
    pthread_t     os_thread;
    int           vp_id;
    tcb_t        *local_head;
    tcb_t        *local_tail;
    int           local_count;
    spinlock_t    queue_lock;
    tcb_t        *current;
    ucontext_t    sched_context;
    volatile int  running;
} virtual_processor_t;

static virtual_processor_t vps[MAX_VIRTUAL_PROCS];
static int num_vps = 0;
static __thread int my_vp_id = 0;
static __thread virtual_processor_t *my_vp = NULL;

static void vp_enqueue(virtual_processor_t *vp, tcb_t *tcb) {
    spinlock_acquire(&vp->queue_lock);
    tcb->next = NULL;
    tcb->vp_id = vp->vp_id;
    if (vp->local_tail) vp->local_tail->next = tcb;
    else                vp->local_head = tcb;
    vp->local_tail = tcb;
    vp->local_count++;
    spinlock_release(&vp->queue_lock);
}

static tcb_t* vp_dequeue(virtual_processor_t *vp) {
    spinlock_acquire(&vp->queue_lock);
    tcb_t *tcb = vp->local_head;
    if (tcb) {
        vp->local_head = tcb->next;
        if (!vp->local_head) vp->local_tail = NULL;
        tcb->next = NULL;
        vp->local_count--;
    }
    spinlock_release(&vp->queue_lock);
    return tcb;
}

static tcb_t* try_steal(virtual_processor_t *thief) {
    if (num_vps <= 1) return NULL;

    unsigned int seed = (unsigned int)(thief->vp_id * 31 + get_time_ns());
    int start = seed % num_vps;

    for (int i = 0; i < num_vps; i++) {
        int vid = (start + i) % num_vps;
        if (vid == thief->vp_id) continue;

        virtual_processor_t *victim = &vps[vid];
        spinlock_acquire(&victim->queue_lock);

        if (victim->local_count <= 1) {
            spinlock_release(&victim->queue_lock);
            continue;
        }

        int steal_count = victim->local_count / 2;
        tcb_t *stolen_head = NULL;
        tcb_t *stolen_tail = NULL;

        for (int j = 0; j < steal_count; j++) {
            tcb_t *t = victim->local_head;
            if (!t) break;
            victim->local_head = t->next;
            victim->local_count--;

            t->next = NULL;
            t->vp_id = thief->vp_id;
            if (stolen_tail) stolen_tail->next = t;
            else             stolen_head = t;
            stolen_tail = t;
        }
        if (!victim->local_head) victim->local_tail = NULL;
        spinlock_release(&victim->queue_lock);

        if (stolen_head) {
            spinlock_acquire(&thief->queue_lock);
            if (thief->local_tail) thief->local_tail->next = stolen_head;
            else                   thief->local_head = stolen_head;
            thief->local_tail = stolen_tail;
            thief->local_count += steal_count;
            spinlock_release(&thief->queue_lock);

            trace_emit(stolen_head->thread_id, TRACE_STEAL, thief->vp_id);
            return vp_dequeue(thief);
        }
    }
    return NULL;
}

/* M:N yield — return to VP scheduler context */
static void mn_yield(void) {
    tcb_t *prev = current_thread;
    virtual_processor_t *vp = my_vp;

    if (prev->state == UTHREAD_RUNNING) {
        prev->state = UTHREAD_READY;
        trace_emit(prev->thread_id, TRACE_YIELD, vp->vp_id);
        vp_enqueue(vp, prev);
    }

    swapcontext(TCB_CONTEXT(prev), &vp->sched_context);
}

/* M:N exit — mark finished, return to VP scheduler */
static void mn_exit(void) {
    tcb_t *prev = current_thread;
    prev->state = UTHREAD_FINISHED;
    trace_emit(prev->thread_id, TRACE_EXIT, my_vp_id);
    setcontext(&my_vp->sched_context);
}

static void* vp_loop(void *arg) {
    virtual_processor_t *vp = (virtual_processor_t*)arg;
    my_vp_id = vp->vp_id;
    my_vp = vp;

    /* Block SIGALRM — M:N mode doesn't use timer preemption */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGALRM);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    while (vp->running) {
        tcb_t *next = vp_dequeue(vp);

        if (!next) {
            spinlock_acquire(&ready_lock);
            next = dequeue();
            spinlock_release(&ready_lock);
        }

        if (!next) next = try_steal(vp);

        if (!next) {
            int alive = 0;
            spinlock_acquire(&global_list_lock);
            tcb_t *t = global_thread_list;
            while (t) {
                if (t->state != UTHREAD_FINISHED) { alive = 1; break; }
                t = t->global_next;
            }
            spinlock_release(&global_list_lock);
            if (!alive) break;
            usleep(50);
            continue;
        }

        vp->current = next;
        current_thread = next;
        next->state = UTHREAD_RUNNING;
        next->vp_id = vp->vp_id;
        trace_emit(next->thread_id, TRACE_RUN, vp->vp_id);

        swapcontext(&vp->sched_context, TCB_CONTEXT(next));

        vp->current = NULL;
    }

    return NULL;
}

/* =========================================================================
 * THREAD WRAPPER
 * ========================================================================= */

static void thread_wrapper(void) {
    unblock_signals();
    tcb_t *tcb = current_thread;
    tcb->fn(tcb->arg);
    uthread_exit();
}

/* =========================================================================
 * CORE API
 * ========================================================================= */

void uthread_init(void) {
    spinlock_init(&global_list_lock);
    spinlock_init(&ready_lock);
    spinlock_init(&sleep_list_lock);

    global_thread_list = NULL;
    ready_head = NULL;
    ready_tail = NULL;
    ready_count = 0;
    atomic_store(&next_thread_id, 1);
    creation_counter = 0;
    current_scheduler = NULL;
    mn_mode = 0;

    /* Main thread TCB */
    tcb_t *main_tcb = alloc_tcb();
    if (!main_tcb) {
        fprintf(stderr, "uthread_init: failed to allocate main TCB\n");
        exit(1);
    }
    main_tcb->thread_id = 0;
    main_tcb->state = UTHREAD_RUNNING;
    main_tcb->creation_order = creation_counter++;
    main_tcb->stack = NULL;
    main_tcb->stack_alloc = 0;
    getcontext(TCB_CONTEXT(main_tcb));

    global_list_add(main_tcb);
    current_thread = main_tcb;

    /* Preemption timer */
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

    /* Guard page SIGSEGV handler (Phase 11) */
    struct sigaction sa_segv;
    sa_segv.sa_sigaction = sigsegv_handler;
    sigemptyset(&sa_segv.sa_mask);
    sa_segv.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa_segv, NULL);

    /* Epoll (Phase 12) */
    epoll_fd = epoll_create1(0);
}

int uthread_create_with_priority(void (*fn)(void*), void *arg, int priority) {
    block_signals();
    spinlock_acquire(&global_list_lock);
    gc_finished_threads();
    spinlock_release(&global_list_lock);

    tcb_t *tcb = alloc_tcb();
    if (!tcb) {
        unblock_signals();
        return -1;
    }

    tcb->thread_id = atomic_fetch_add(&next_thread_id, 1);
    tcb->state = UTHREAD_READY;
    tcb->fn = fn;
    tcb->arg = arg;
    tcb->priority = priority;
    tcb->creation_order = atomic_fetch_add(&creation_counter, 1);

    /* Guarded stack (Phase 11) */
    tcb->stack = alloc_guarded_stack(&tcb->stack_alloc);
    if (!tcb->stack) {
        free(tcb);
        unblock_signals();
        return -1;
    }

    getcontext(TCB_CONTEXT(tcb));
    TCB_CONTEXT(tcb)->uc_stack.ss_sp = (char*)tcb->stack + GUARD_PAGE_SIZE;
    TCB_CONTEXT(tcb)->uc_stack.ss_size = UTHREAD_STACK_SIZE;
    TCB_CONTEXT(tcb)->uc_link = NULL;
    makecontext(TCB_CONTEXT(tcb), thread_wrapper, 0);

    spinlock_acquire(&global_list_lock);
    global_list_add(tcb);
    spinlock_release(&global_list_lock);

    if (mn_mode) {
        /* In M:N mode, add to global ready queue for VPs to pick up */
        spinlock_acquire(&ready_lock);
        enqueue(tcb);
        spinlock_release(&ready_lock);
    } else {
        enqueue(tcb);
    }

    trace_emit(tcb->thread_id, TRACE_CREATE, 0);

    unblock_signals();
    return tcb->thread_id;
}

int uthread_create(void (*fn)(void*), void *arg) {
    return uthread_create_with_priority(fn, arg, 0);
}

void uthread_yield(void) {
    /* M:N dispatch */
    if (mn_mode) {
        mn_yield();
        return;
    }

    block_signals();
    gc_finished_threads();

    check_io_events();
    check_sleep_list();

    tcb_t *prev = current_thread;
    if (prev->state == UTHREAD_RUNNING) {
        prev->state = UTHREAD_READY;
        trace_emit(prev->thread_id, TRACE_YIELD, 0);
        enqueue(prev);
    }

    tcb_t *next = schedule_next();
    if (next == NULL) {
        if (prev->state == UTHREAD_READY) {
            prev->state = UTHREAD_RUNNING;
            remove_from_ready_queue(prev);
            trace_emit(prev->thread_id, TRACE_RUN, 0);
            unblock_signals();
            return;
        } else {
            /* Check for I/O-blocked or sleeping threads */
            tcb_t *t = global_thread_list;
            int has_live = 0;
            while (t) {
                if (t->state != UTHREAD_FINISHED) { has_live = 1; break; }
                t = t->global_next;
            }
            if (has_live && epoll_fd >= 0) {
                unblock_signals();
                while (1) {
                    block_signals();
                    check_io_events();
                    check_sleep_list();
                    next = schedule_next();
                    if (next) break;
                    unblock_signals();
                    usleep(100);
                }
            } else {
                uthread_dump_trace("trace.json");
                exit(0);
            }
        }
    }

    current_thread = next;
    next->state = UTHREAD_RUNNING;
    trace_emit(next->thread_id, TRACE_RUN, 0);

    swapcontext(TCB_CONTEXT(prev), TCB_CONTEXT(next));
    unblock_signals();
}

void uthread_exit(void) {
    /* M:N dispatch */
    if (mn_mode) {
        mn_exit();
        return;
    }

    block_signals();

    tcb_t *prev = current_thread;
    prev->state = UTHREAD_FINISHED;
    trace_emit(prev->thread_id, TRACE_EXIT, 0);

    check_io_events();
    check_sleep_list();

    tcb_t *next = schedule_next();
    if (next == NULL) {
        /* Check for I/O-blocked or sleeping threads */
        tcb_t *t = global_thread_list;
        int has_live = 0;
        while (t) {
            if (t->state != UTHREAD_FINISHED) { has_live = 1; break; }
            t = t->global_next;
        }
        if (has_live && epoll_fd >= 0) {
            unblock_signals();
            while (1) {
                block_signals();
                check_io_events();
                check_sleep_list();
                next = schedule_next();
                if (next) break;
                unblock_signals();
                usleep(100);
            }
        } else {
            gc_finished_threads();
            uthread_dump_trace("trace.json");
            exit(0);
        }
    }

    current_thread = next;
    next->state = UTHREAD_RUNNING;
    trace_emit(next->thread_id, TRACE_RUN, 0);

    swapcontext(TCB_CONTEXT(prev), TCB_CONTEXT(next));
}

int uthread_self(void) {
    return current_thread ? current_thread->thread_id : -1;
}

void uthread_set_scheduler(scheduler_fn policy) {
    current_scheduler = policy;
}

/* =========================================================================
 * MUTEX API (linked-list wait queue)
 * ========================================================================= */

void uthread_mutex_init(uthread_mutex_t *m) {
    m->locked = 0;
    m->wait_head = NULL;
    m->wait_tail = NULL;
}

void uthread_mutex_lock(uthread_mutex_t *m) {
    block_signals();
    if (!m->locked) {
        m->locked = 1;
        unblock_signals();
        return;
    }

    current_thread->state = UTHREAD_BLOCKED;
    current_thread->next = NULL;
    trace_emit(current_thread->thread_id, TRACE_BLOCK, 0);

    if (m->wait_tail) m->wait_tail->next = current_thread;
    else              m->wait_head = current_thread;
    m->wait_tail = current_thread;

    uthread_yield();
}

void uthread_mutex_unlock(uthread_mutex_t *m) {
    block_signals();
    if (m->wait_head) {
        tcb_t *woken = m->wait_head;
        m->wait_head = woken->next;
        if (!m->wait_head) m->wait_tail = NULL;
        woken->next = NULL;
        woken->state = UTHREAD_READY;
        trace_emit(woken->thread_id, TRACE_UNBLOCK, 0);
        enqueue(woken);
    } else {
        m->locked = 0;
    }
    unblock_signals();
}

/* =========================================================================
 * SEMAPHORE API (linked-list wait queue)
 * ========================================================================= */

void uthread_sem_init(uthread_sem_t *s, int initial_count) {
    s->count = initial_count;
    s->wait_head = NULL;
    s->wait_tail = NULL;
}

void uthread_sem_wait(uthread_sem_t *s) {
    block_signals();
    if (s->count > 0) {
        s->count--;
        unblock_signals();
        return;
    }

    current_thread->state = UTHREAD_BLOCKED;
    current_thread->next = NULL;
    trace_emit(current_thread->thread_id, TRACE_BLOCK, 0);

    if (s->wait_tail) s->wait_tail->next = current_thread;
    else              s->wait_head = current_thread;
    s->wait_tail = current_thread;

    uthread_yield();
}

void uthread_sem_post(uthread_sem_t *s) {
    block_signals();
    s->count++;
    if (s->wait_head) {
        s->count--;
        tcb_t *woken = s->wait_head;
        s->wait_head = woken->next;
        if (!s->wait_head) s->wait_tail = NULL;
        woken->next = NULL;
        woken->state = UTHREAD_READY;
        trace_emit(woken->thread_id, TRACE_UNBLOCK, 0);
        enqueue(woken);
    }
    unblock_signals();
}

/* =========================================================================
 * CHANNEL API
 * ========================================================================= */

void uthread_channel_init(uthread_channel_t *ch, int capacity) {
    if (capacity > UTHREAD_CHANNEL_MAX_CAPACITY)
        capacity = UTHREAD_CHANNEL_MAX_CAPACITY;
    ch->capacity = capacity;
    ch->head = 0;
    ch->tail = 0;
    uthread_sem_init(&ch->space_available, capacity);
    uthread_sem_init(&ch->messages_available, 0);
    uthread_mutex_init(&ch->mutex);
}

void uthread_channel_send(uthread_channel_t *ch, void *msg) {
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

/* =========================================================================
 * ASYNC I/O API (Phase 12)
 * ========================================================================= */

void uthread_sleep(unsigned int ms) {
    block_signals();

    uint64_t now = get_time_ns();
    current_thread->sleep_until_ns = now + (uint64_t)ms * 1000000ULL;
    current_thread->state = UTHREAD_IO_BLOCKED;
    trace_emit(current_thread->thread_id, TRACE_SLEEP, 0);

    spinlock_acquire(&sleep_list_lock);
    current_thread->next = sleep_list_head;
    sleep_list_head = current_thread;
    spinlock_release(&sleep_list_lock);

    uthread_yield();
}

ssize_t uthread_read(int fd, void *buf, size_t count) {
    set_nonblocking(fd);
    ssize_t ret = read(fd, buf, count);
    if (ret >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
        return ret;

    block_signals();
    current_thread->io_fd = fd;
    current_thread->io_events = EPOLLIN;
    current_thread->state = UTHREAD_IO_BLOCKED;
    trace_emit(current_thread->thread_id, TRACE_IO_BLOCK, 0);

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = current_thread;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);

    uthread_yield();

    current_thread->io_fd = -1;
    return read(fd, buf, count);
}

ssize_t uthread_write(int fd, const void *buf, size_t count) {
    set_nonblocking(fd);
    ssize_t ret = write(fd, buf, count);
    if (ret >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
        return ret;

    block_signals();
    current_thread->io_fd = fd;
    current_thread->io_events = EPOLLOUT;
    current_thread->state = UTHREAD_IO_BLOCKED;
    trace_emit(current_thread->thread_id, TRACE_IO_BLOCK, 0);

    struct epoll_event ev;
    ev.events = EPOLLOUT | EPOLLET;
    ev.data.ptr = current_thread;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);

    uthread_yield();

    current_thread->io_fd = -1;
    return write(fd, buf, count);
}

/* =========================================================================
 * TRACING API (Phase 13)
 * ========================================================================= */

void uthread_trace_enable(void) {
    if (!trace_buffer)
        trace_buffer = calloc(TRACE_BUFFER_SIZE, sizeof(trace_event_t));
    atomic_store(&trace_count, 0);
    trace_enabled = 1;
}

void uthread_trace_disable(void) {
    trace_enabled = 0;
}

void uthread_dump_trace(const char *filename) {
    if (!trace_buffer) return;
    int count = atomic_load(&trace_count);
    if (count <= 0) return;
    if (count > TRACE_BUFFER_SIZE) count = TRACE_BUFFER_SIZE;

    FILE *f = fopen(filename, "w");
    if (!f) return;

    fprintf(f, "{\"traceEvents\":[\n");
    for (int i = 0; i < count; i++) {
        trace_event_t *ev = &trace_buffer[i];
        fprintf(f, "%s{\"name\":\"%s\",\"cat\":\"sched\","
                "\"ph\":\"%s\",\"ts\":%.3f,"
                "\"pid\":0,\"tid\":%d,"
                "\"args\":{\"core\":%d}}",
                (i > 0) ? ",\n" : "",
                trace_event_name(ev->type),
                trace_event_phase(ev->type),
                (double)ev->timestamp_ns / 1000.0,
                ev->tid,
                ev->core_id);
    }
    fprintf(f, "\n]}\n");
    fclose(f);
}

/* =========================================================================
 * M:N INIT API (Phase 14)
 * ========================================================================= */

void uthread_init_mn(int num_processors) {
    if (num_processors < 1) num_processors = 1;
    if (num_processors > MAX_VIRTUAL_PROCS) num_processors = MAX_VIRTUAL_PROCS;

    spinlock_init(&global_list_lock);
    spinlock_init(&ready_lock);
    spinlock_init(&sleep_list_lock);

    global_thread_list = NULL;
    ready_head = NULL;
    ready_tail = NULL;
    ready_count = 0;
    atomic_store(&next_thread_id, 0);
    creation_counter = 0;
    current_scheduler = NULL;
    mn_mode = 1;
    num_vps = num_processors;

    /* SIGSEGV handler */
    struct sigaction sa_segv;
    sa_segv.sa_sigaction = sigsegv_handler;
    sigemptyset(&sa_segv.sa_mask);
    sa_segv.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa_segv, NULL);

    /* Epoll */
    epoll_fd = epoll_create1(0);

    /* Disable timer — M:N mode uses cooperative scheduling per VP */
    struct itimerval it;
    memset(&it, 0, sizeof(it));
    setitimer(ITIMER_REAL, &it, NULL);

    for (int i = 0; i < num_processors; i++) {
        vps[i].vp_id = i;
        vps[i].local_head = NULL;
        vps[i].local_tail = NULL;
        vps[i].local_count = 0;
        spinlock_init(&vps[i].queue_lock);
        vps[i].current = NULL;
        vps[i].running = 1;
    }
}

/* Start the M:N runtime — spawns VP pthreads and blocks until all user threads finish */
void uthread_mn_run(void) {
    /* VPs 1..N-1 are pthreads; VP 0 runs on the calling (main) thread */
    for (int i = 1; i < num_vps; i++)
        pthread_create(&vps[i].os_thread, NULL, vp_loop, &vps[i]);

    vp_loop(&vps[0]);

    for (int i = 1; i < num_vps; i++) {
        vps[i].running = 0;
        pthread_join(vps[i].os_thread, NULL);
    }

    uthread_dump_trace("trace.json");
    gc_finished_threads();
}
