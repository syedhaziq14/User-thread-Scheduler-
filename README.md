# uthread: A User-Space Thread Scheduling Library

## Project Overview and Motivation
`uthread` is a lightweight, user-space thread scheduling library implemented in C. 

Modern high-performance concurrent systems—such as Go's Goroutine scheduler and Java's Project Loom (Virtual Threads)—rely heavily on M:N threading models or purely user-space threading (green threads) to achieve massive scalability. Native OS kernel threads (`pthreads`) are relatively heavy: they require system calls to spawn, consume large default stack sizes, and trap into the kernel for context switching.

`uthread` was built from scratch to explore and demonstrate the mechanics powering these modern concurrency frameworks. By managing thread context switches entirely in user-space, `uthread` achieves sub-microsecond context switch latencies and can spawn thousands of threads in a fraction of the time it takes the OS kernel. 

## Architecture
```text
+-------------------------------------------------------------+
|                      Demo Applications                      |
|  (Producer/Consumer, Preemption Demo, Message Pipelines)    |
+-------------------------------------------------------------+
                               |
+-------------------------------------------------------------+
|              High-Level Synchronization APIs                |
|      (uthread_mutex_t, uthread_sem_t, uthread_channel_t)    |
+-------------------------------------------------------------+
                               |
+-------------------------------------------------------------+
|                    Scheduler Core Engine                    |
|   (Ready Queue, Priority/RR Policies, TCB State Machine)    |
+-------------------------------------------------------------+
                               |
+-------------------------------------------------------------+
|           Hardware Context & Preemption Abstraction         |
|        (ucontext_t, SIGALRM ITIMER_REAL signals)            |
+-------------------------------------------------------------+
                               |
+-------------------------------------------------------------+
|                         OS Kernel                           |
|      (Single native process providing CPU execution)        |
+-------------------------------------------------------------+
```

## Build and Run Instructions
The project uses a standard `Makefile` and requires a Linux environment (or WSL) due to its reliance on `ucontext.h`.

**Dependencies**: `gcc`, `make`

**Build & Run Tests**:
```bash
make clean
make test

# Run individual tests:
./tests/test_interleave
./tests/test_scheduler prio
./tests/test_mutex lock
./tests/test_semaphore
./tests/test_preempt
./tests/test_channel
```

**Build & Run Benchmarks**:
```bash
make bench
./benchmarks/benchmarks
```

## Benchmark Results
The following benchmarks were recorded locally, comparing `uthread` against native Linux `pthreads`:

| System  | Benchmark                      | Time (s)     | Iterations |
|---------|--------------------------------|--------------|------------|
| uthread | Context Switch Latency         | 0.590766     | 100000     |
| pthread | Context Switch Latency         | 4.582479     | 100000     |
| uthread | Throughput (1000 threads)      | 0.014724     | 1000       |
| pthread | Throughput (1000 threads)      | 0.240703     | 1000       |
| uthread | Mutex Contention (8 threads)   | 0.350653     | 80000      |
| pthread | Mutex Contention (8 threads)   | 0.004606     | 80000      |

**Key Takeaways**:
- `uthread` is significantly faster at context switching and thread creation because it avoids kernel-mode traps and heavy OS resource allocation.
- `pthread` vastly outperforms `uthread` on multi-core mutex contention because Linux pthreads utilize fast user-space atomics (Futexes) and true parallelism, whereas `uthread` must perform a full context-switch every time it encounters a blocked mutex.

## Known Limitations
- **Single-Core Execution**: This is a 1:N threading model. All user threads are multiplexed onto a single native kernel thread. It does not utilize multiple CPU cores for parallel execution.
- **Fixed Thread Limit**: The system supports a hardcoded maximum of 1,024 concurrent threads.
- **Blocking System Calls**: Because it runs on a single OS thread, if a `uthread` invokes a blocking I/O system call (like `read()`), the entire runtime and all other `uthreads` are stalled. (Real systems like Go solve this via asynchronous I/O polling).
- **Fixed Stack Size**: Stacks are allocated at a fixed 64KB size and do not dynamically grow. Stack overflows will result in segmentation faults.
