# The `uthread` Codebase Tour & API Reference

Welcome to the internal documentation for the `uthread` project. This guide serves as a comprehensive "tour" of the repository, explaining every file, folder, and the core functions that make up the user-space scheduling engine.

---

## 1. Project Structure

```text
User-thread-Scheduler-/
├── include/
│   └── uthread.h             # The public API and Data Structures (TCB)
├── src/
│   └── uthread.c             # The core engine (Scheduler, Context Switching, I/O)
├── benchmarks/               # Performance measurement scripts
│   ├── benchmarks.c          # 1:N mode benchmarks
│   ├── bench_worksteal.c     # M:N mode benchmarks
│   └── bench_demo.c          # Massive Concurrency classroom demonstration
├── tests/                    # Unit tests for all phases (Phases 1-14)
├── tools/
│   └── gantt_viewer.html     # Interactive trace visualizer
├── docs/                     # Additional documentation
└── Makefile                  # Build script for compiling the project
```

---

## 2. Core Engine Files

### `include/uthread.h`
This is the **Public API header**. Any application developer that wants to use your library will `#include "uthread.h"`. 

**Key Data Structures:**
- `struct tcb` (Thread Control Block): The brain of every thread. It stores the thread's ID, current state (`READY`, `RUNNING`, `BLOCKED`), a pointer to its `mmap`'d memory stack, its scheduling priority, and intrusive linked-list pointers so it can be placed in Ready Queues or Wait Queues without calling `malloc()`.
- `uthread_mutex_t` / `uthread_sem_t`: Thread-safe synchronization primitives that integrate directly with the scheduler to avoid freezing the OS thread.

### `src/uthread.c`
This is the **Core Implementation file** (over 1,100 lines of C code). It is where the magic happens. It handles everything from hardware context switching (saving/restoring CPU registers via `ucontext.h`) to intercepting blocking I/O calls via Linux `epoll`.

---

## 3. The API (Function-by-Function Breakdown)

### Core Thread Management
- `void uthread_init(void)`: Initializes the single-core (1:N) scheduler environment. It creates a Main TCB to represent the original OS process and sets up the global Ready Queue.
- `int uthread_create(void (*fn)(void*), void *arg)`: The workhorse. It asks the OS for 64KB of memory via `mmap`, sets up an isolated stack, links it to the function `fn`, assigns a Thread ID, and pushes it onto the Ready Queue. Returns the Thread ID (or -1 if memory is exhausted).
- `void uthread_yield(void)`: Voluntarily pauses the current running thread. It saves the CPU registers using `swapcontext()`, places the thread at the back of the Ready Queue, and immediately jumps into the code of the next available thread.
- `void uthread_exit(void)`: Marks the current thread as `FINISHED`. The scheduler cleans up its memory (stack) and moves on to the next thread.
- `int uthread_self(void)`: Returns the integer Thread ID of whatever thread is currently running.

### M:N Work Stealing Architecture
- `void uthread_init_mn(int num_vps)`: Initializes the advanced multi-core mode. Instead of one Ready Queue, it creates `num_vps` separate Queues and spawns native `pthreads` (Virtual Processors) to manage them.
- `void uthread_mn_run(void)`: Starts the multi-core engine. The Virtual Processors will begin popping threads off their local queues. If a Virtual Processor runs out of work, it will silently **"Steal"** threads from the queues of other Virtual Processors to keep the CPU 100% utilized.

### Synchronization (Mutexes & Channels)
- `void uthread_mutex_lock(uthread_mutex_t *m)`: Attempts to grab a lock. If it's already taken, it *does not spin*. Instead, it changes its state to `BLOCKED`, adds itself to the Mutex's internal wait list, and yields the CPU.
- `void uthread_mutex_unlock(uthread_mutex_t *m)`: Releases the lock and immediately pops the first waiting thread off the wait list, putting it back onto the Ready Queue.
- `void uthread_channel_send(ch, msg)` / `uthread_channel_recv(ch)`: Implements Go-style channels. Threads can safely pass data (messages) to each other. If a thread calls `recv` and no message is there, it goes to sleep until another thread calls `send`.

### Asynchronous I/O (The High-Scale Secret)
- `ssize_t uthread_read(int fd, void *buf, size_t count)`: A custom wrapper around the standard `read()`. If the file or network socket isn't ready to be read, the thread tells the Linux Kernel (`epoll`) to watch the file descriptor, and then puts itself to sleep (`UTHREAD_IO_BLOCKED`). The Virtual Processor immediately runs another thread. When the data arrives over the network, `epoll` wakes the thread back up.

### Observability & Tracing
- `void uthread_trace_enable(void)`: Turns on the nanosecond-precision event logger. Every time a thread yields, blocks, or steals work, it records a timestamp.
- `void uthread_dump_trace(const char *filename)`: Dumps all the recorded events into a `.json` file formatted for the Google Chrome Trace Viewer format.

---

## 4. Supporting Directories

### `/benchmarks`
- **`benchmarks.c`**: Evaluates the raw speed of the 1:N single-core scheduler against `pthreads`.
- **`bench_worksteal.c`**: Computes primes across multiple CPU cores to prove that the M:N scheduler actually achieves true parallelism and scales linearly.
- **`bench_demo.c`**: The "Mic-Drop" classroom presentation script. It attempts to spawn 15,000 threads. The OS struggles heavily, while `uthread` processes it instantly, proving extreme concurrency superiority.

### `/tools`
- **`gantt_viewer.html`**: A custom-built HTML/JavaScript web application. You drag and drop the `trace.json` file generated by `uthread_dump_trace()` into this page, and it renders a beautiful, interactive timeline showing exactly what every CPU core and every thread was doing down to the nanosecond.

### `/tests`
Contains over a dozen isolated unit tests (e.g., `test_mutex.c`, `test_guard_page.c`, `test_async_io.c`). These are executed via `make test` to guarantee that every new feature (Phases 1-14) doesn't break older features during active development.
