# uthread: Complete Codebase Tour & API Reference

> A full function-by-function, file-by-file breakdown of every C file, shell script, and tool in the `uthread` project.

---

## Table of Contents
1. [Project Structure Overview](#1-project-structure-overview)
2. [Core Library](#2-core-library)
   - [include/uthread.h](#includeuthreadh)
   - [src/uthread.c](#srcuthreadc)
3. [Tests Directory](#3-tests-directory)
4. [Shell Scripts](#4-shell-scripts)
5. [Benchmarks Directory](#5-benchmarks-directory)
6. [Tools Directory](#6-tools-directory)
7. [Build System](#7-build-system-makefile)

---

## 1. Project Structure Overview

```text
User-thread-Scheduler-/
├── include/
│   └── uthread.h                  # Public API header (data structures + function prototypes)
├── src/
│   └── uthread.c                  # Core 1173-line engine implementation
├── tests/
│   ├── test_basic.c               # Phase 1: Sanity check — library initializes
│   ├── test_interleave.c          # Phase 2: Round-robin cooperative scheduling
│   ├── test_mutex.c               # Phase 3: Mutex correctness (race condition demo)
│   ├── test_semaphore.c           # Phase 4: Producer-Consumer with semaphores
│   ├── test_preempt.c             # Phase 5: Preemptive scheduling via SIGALRM
│   ├── test_scheduler.c           # Phase 6: Pluggable scheduling policies
│   ├── test_channel.c             # Phase 7: Go-style concurrent pipeline channels
│   ├── test_guard_page.c          # Phase 11: Memory-safe stacks (no overflow)
│   ├── test_guard_page_overflow.c # Phase 11: Intentional overflow — guard page fires
│   ├── test_infinite_threads.c    # Phase 10: Dynamic thread allocation (10,000 threads)
│   ├── test_async_io.c            # Phase 12: Non-blocking async I/O & sleep
│   ├── test_worksteal.c           # Phase 14: M:N Work Stealing multi-core engine
│   ├── run_mutex_test.sh          # Shell script: automated mutex comparison demo
│   └── run_semaphore_test.sh      # Shell script: automated semaphore correctness verifier
├── benchmarks/
│   ├── benchmarks.c               # 1:N Benchmarks: context switch, throughput, mutex
│   ├── bench_worksteal.c          # M:N Benchmarks: prime counting, context latency
│   └── bench_demo.c               # Class Demo: 15,000 thread spawning race
├── tools/
│   └── gantt_viewer.html          # Interactive timeline visualizer for trace.json
├── docs/                          # Additional documentation and analysis files
└── Makefile                       # Build system
```

---

## 2. Core Library

### `include/uthread.h`
**Role:** The single public-facing header. Any developer using this library only needs to `#include "uthread.h"`. It exposes all data structures, type definitions, and function prototypes without leaking internal implementation details.

#### Key Constants
| Constant | Default Value | Purpose |
|---|---|---|
| `UTHREAD_STACK_SIZE` | `64 * 1024` (64 KB) | Memory allocated per green thread via `mmap`. Override with `-DUTHREAD_STACK_SIZE=N`. |
| `TIME_QUANTUM_MS` | `10` | Timer interrupt interval (ms) for preemption via `SIGALRM`. |
| `UTHREAD_CHANNEL_MAX_CAPACITY` | `64` | Maximum messages a channel can buffer. |

#### `uthread_state_t` (enum)
Represents the lifecycle of every green thread:
- `UTHREAD_READY` — Queued and waiting to run.
- `UTHREAD_RUNNING` — Currently executing on a Virtual Processor.
- `UTHREAD_BLOCKED` — Waiting on a Mutex or Semaphore.
- `UTHREAD_IO_BLOCKED` — Waiting for a network/file event via `epoll`.
- `UTHREAD_FINISHED` — Execution complete, memory can be reclaimed.

#### `struct tcb` (Thread Control Block)
The core data structure. Every green thread in existence is represented by one `tcb_t` instance:

| Field | Type | Purpose |
|---|---|---|
| `thread_id` | `int` | Unique integer ID assigned at creation. |
| `state` | `uthread_state_t` | Current lifecycle state. |
| `stack` | `void *` | Pointer to the base of the thread's `mmap`'d memory region (includes guard page). |
| `stack_alloc` | `size_t` | Total allocated size: `GUARD_PAGE_SIZE + UTHREAD_STACK_SIZE`. |
| `fn` | `void (*)(void*)` | Function pointer — the code this thread will run. |
| `arg` | `void *` | Argument passed to `fn` at thread start. |
| `priority` | `int` | Integer priority (lower number = higher priority). Used by the priority scheduler. |
| `creation_order` | `int` | Monotonically increasing counter for tie-breaking in priority mode. |
| `next` | `tcb_t *` | Intrusive linked-list pointer for Ready Queue and Wait Queue membership. |
| `global_next/prev` | `tcb_t *` | Doubly-linked list pointers for the global thread registry (used for GC and tracing). |
| `io_fd` | `int` | File descriptor being watched by `epoll` (`-1` if none). |
| `io_events` | `int` | `epoll` event mask (`EPOLLIN`, `EPOLLOUT`) waiting to fire. |
| `io_result` | `ssize_t` | Bytes read/written, filled in by the I/O completion handler. |
| `sleep_until_ns` | `unsigned long long` | Nanosecond timestamp when the thread should wake from `uthread_sleep()` (`0` = not sleeping). |
| `vp_id` | `int` | ID of the Virtual Processor currently running this thread (M:N mode). |

#### Synchronization Structs
- **`uthread_mutex_t`**: Contains a `locked` flag and a wait queue (`wait_head`/`wait_tail`) of threads blocked on this mutex.
- **`uthread_sem_t`**: Contains an integer `count` and a wait queue of blocked threads. Threads sleep when `count == 0`.
- **`uthread_channel_t`**: A buffered channel (like Go's channels). Contains a circular `buffer[]` array, head/tail indices, a `space_available` semaphore (how many slots free), a `messages_available` semaphore (how many messages ready), and a `mutex` protecting concurrent access.

---

### `src/uthread.c`
**Role:** The 1,173-line engine. This file is compiled into a `.o` object and linked with every test and benchmark.

#### Internal-Only Types (not in public header)
- **`tcb_internal_t`**: A wrapper around `tcb_t` that appends a `ucontext_t` field. `ucontext_t` holds the CPU register state (program counter, stack pointer, general-purpose registers). It is kept internal so that `ucontext.h` system header is not forced upon library users.
- **`spinlock_t`**: A simple atomic spinlock used to protect shared queues in M:N mode. Contains one `atomic_int`. Uses `x86 PAUSE` instruction for low-latency spinning, and falls back to `sched_yield()` after 100 failed attempts to prevent cache-line thrashing across CPU cores.
- **`trace_event_t`**: One recorded scheduler event. Contains the thread ID, event type (`TRACE_CREATE`, `TRACE_RUN`, `TRACE_STEAL`, etc.), a nanosecond timestamp, and the core ID it occurred on.

#### Internal Functions

**Signal & Preemption Management**
- **`block_signals()`** / **`unblock_signals()`**: In 1:N mode, masks `SIGALRM` around critical sections (queue pushes/pops) to prevent preemption mid-operation. Not used in M:N mode (which relies on spinlocks instead).
- **`alrm_handler(int signum)`**: The `SIGALRM` handler. Fires every `TIME_QUANTUM_MS` milliseconds (default: 10ms). Its entire body is: `uthread_yield()`. This is how preemption works — the kernel timer interrupts the running thread and forces a context switch.

**Spinlock**
- **`spinlock_init(sl)`**: Sets the atomic lock to `0` (unlocked).
- **`spinlock_acquire(sl)`**: Loops calling `atomic_exchange`. Spins with `PAUSE` for up to 100 iterations, then calls `sched_yield()` to give the OS thread back to the kernel rather than burning CPU cycles indefinitely.
- **`spinlock_release(sl)`**: Atomically sets lock to `0`.

**Tracing Engine**
- **`get_time_ns()`**: Reads `CLOCK_MONOTONIC` and returns nanoseconds as `uint64_t`.
- **`trace_emit(tid, type, core_id)`**: Atomically claims the next index in the circular `trace_buffer[]` and writes a `trace_event_t`. Lock-free — uses `atomic_fetch_add` to claim the slot.
- **`trace_event_name(t)`**: Converts enum value to a human-readable string (`"RUN"`, `"STEAL"`, etc.) for JSON output.
- **`trace_event_phase(t)`**: Returns Chrome Trace Event format phase (`"B"` = begin duration, `"E"` = end duration, `"i"` = instant).

**Scheduler Core**
- **`mn_yield()`**: The internal context switch for M:N mode. Saves the current thread's CPU state and switches to the Virtual Processor's scheduler loop.
- **`mn_exit()`**: Called when an M:N thread's function returns. Marks the thread `FINISHED`, calls `mn_yield()`.
- **`check_io_events()`**: Calls `epoll_wait()` with a 0ms timeout to poll for pending I/O completions. For each ready event, finds the matching `UTHREAD_IO_BLOCKED` thread, fills `io_result`, and moves it back to `READY`.
- **`check_sleep_list()`**: Scans all threads in the global list. For any thread in `UTHREAD_IO_BLOCKED` state with a `sleep_until_ns` that has already passed, marks it `READY` and re-queues it.

---

## 3. Tests Directory

### `test_basic.c` — Phase 1: Sanity Check
**What it tests:** That calling `uthread_init()` doesn't crash or corrupt memory.  
**Functions inside:**
- `main()`: Prints "Initializing...", calls `uthread_init()`, prints "Done." That's it.  
**How to run:** `./tests/test_basic`

---

### `test_interleave.c` — Phase 2: Cooperative Round-Robin
**What it tests:** That multiple threads cooperatively interleave execution. Proves the Round-Robin scheduler is working — threads take turns in order.

**Functions inside:**
- `worker(void *arg)`: Receives its integer ID. Runs 3 iterations. On each iteration, prints its ID and calls `uthread_yield()` to hand control to the next thread.
- `main()`: Calls `uthread_init()`, creates 4 threads each with its own ID, then calls `uthread_exit()` to hand control to the scheduler.

**Expected output:** Thread 1 → Thread 2 → Thread 3 → Thread 4 → Thread 1 → Thread 2 → ...  
**How to run:** `./tests/test_interleave`

---

### `test_mutex.c` — Phase 3: Mutex Correctness
**What it tests:** That the mutex actually eliminates race conditions. Can be run in two modes: `nolock` (shows the bug) and `lock` (shows the fix).

**Functions inside:**
- `worker(void *arg)`: Runs 100,000 iterations. Each iteration reads `counter`, optionally yields (simulating preemption), then writes back `counter + 1`. The yield between read and write is what creates the race condition without a lock.
- `main(int argc, char *argv[])`: Parses `"lock"` or `"nolock"` argument. Initializes mutex. Creates 4 worker threads. Expected final counter: `4 × 100,000 = 400,000`.

**How to run:**
```bash
./tests/test_mutex nolock   # Shows FAIL: lost updates due to race
./tests/test_mutex lock     # Shows PASS: exactly 400,000 every time
```

---

### `test_semaphore.c` — Phase 4: Producer-Consumer
**What it tests:** A classic bounded-buffer problem using two semaphores and a mutex. Proves semaphores correctly synchronize multiple producers and consumers with zero data loss or duplication.

**Key variables:**
- `buffer[BUF_SIZE=5]`: Circular buffer of integers.
- `empty_slots` (semaphore): Starts at 5. Producers wait on this before writing.
- `filled_slots` (semaphore): Starts at 0. Consumers wait on this before reading.
- `buf_mutex`: Protects the buffer index update operations.

**Functions inside:**
- `producer(void *arg)`: Grabs `item_mutex` to get a globally unique item number. Waits on `empty_slots`. Writes to buffer under `buf_mutex`. Signals `filled_slots`. Yields.
- `consumer(void *arg)`: Waits on `filled_slots`. Reads from buffer under `buf_mutex`. Signals `empty_slots`. Yields.
- `main()`: Spawns 2 producers and 2 consumers. Total 20 items must be consumed exactly once.

**How to run:** `./tests/test_semaphore`

---

### `test_preempt.c` — Phase 5: Preemptive Scheduling
**What it tests:** That a CPU-hogging thread that NEVER calls `uthread_yield()` gets forcibly interrupted by the `SIGALRM` timer so that other threads still make progress.

**Functions inside:**
- `infinite_loop_thread(void *arg)`: Contains `while(1)` with heavy CPU math. No `uthread_yield()` anywhere. If preemption is broken, this thread will run forever and starve the others.
- `yielding_thread(void *arg)`: Does some work, prints its status, and calls `uthread_yield()` voluntarily. After 3 iterations for both yielding threads, calls `exit(0)`.
- `main()`: Creates 1 infinite thread and 2 yielding threads.

**How to run:** `./tests/test_preempt`

---

### `test_scheduler.c` — Phase 6: Pluggable Scheduling Policies
**What it tests:** That you can swap the scheduling algorithm at runtime. Compares Round-Robin (equal turn-taking) vs Priority Scheduling (lower number = runs first).

**Functions inside:**
- `worker(void *arg)`: Receives a `worker_arg_t` struct containing ID and priority. Runs 2 iterations, printing both values.
- `main(int argc, char *argv[])`: Accepts `"rr"` or `"prio"` as command-line argument. Calls `uthread_set_scheduler(round_robin_next)` or `uthread_set_scheduler(priority_next)` accordingly. Creates 6 threads with priorities `{3, 1, 2, 0, 2, 1}`.

**How to run:**
```bash
./tests/test_scheduler rr    # All threads interleave equally
./tests/test_scheduler prio  # Priority 0 runs first, then 1s, then 2s, etc.
```

---

### `test_channel.c` — Phase 7: Go-Style Pipeline Channels
**What it tests:** Three-stage concurrent pipeline where threads communicate via typed channels. Data flows: `stage1 → channel_a → stage2 → channel_b → stage3`.

**Functions inside:**
- `stage1(void *arg)`: Produces 10 integer messages and sends them into `channel_a`. Blocks automatically when the channel is full (capacity = 3).
- `stage2(void *arg)`: Receives from `channel_a`, doubles each value, sends into `channel_b`. Blocks when waiting for input.
- `stage3(void *arg)`: Receives from `channel_b`, prints the final result, frees the allocated message, and prints "Pipeline complete!" when all 10 items are processed.
- `main()`: Initializes two channels with capacity 3, creates all 3 stage threads, exits.

**How to run:** `./tests/test_channel`

---

### `test_guard_page.c` — Phase 11: Safe Stack Operation
**What it tests:** That the `mmap` + `mprotect` guard page system doesn't interfere with normal thread execution. Also tests controlled deep recursion that stays within the stack limit.

**Functions inside:**
- `safe_worker(void *arg)`: Runs normally, prints confirmation.
- `multi_frame_worker(void *arg)`: Recursively calls itself 50 times, consuming 256 bytes of stack per frame (total: ~12.8 KB, well within the 64 KB limit). Must complete without a segfault.
- `main()`: Creates both threads and verifies they complete cleanly.

**How to run:** `./tests/test_guard_page`

---

### `test_guard_page_overflow.c` — Phase 11: Stack Overflow Detection
**What it tests:** That a thread which deliberately overflows its stack is killed cleanly by a `SIGSEGV` from the guard page, rather than silently corrupting another thread's memory.

**Functions inside:**
- `stack_overflow_recursive(int depth)`: Allocates 4096 bytes on the stack per frame and recurses infinitely until the guard page boundary is hit.
- `overflow_worker(void *arg)`: Calls `stack_overflow_recursive(0)`.
- `main()`: Uses `fork()` to run the overflow in a child process. The parent waits and checks the exit status. If the child exits with code 139 (SIGSEGV) or is killed by signal 11, the test prints `PASS`.

**How to run:** `./tests/test_guard_page_overflow`

---

### `test_infinite_threads.c` — Phase 10: Dynamic Allocation
**What it tests:** That the library has NO hardcoded thread limit. Creates 10,000 threads dynamically and verifies all complete successfully.

**Functions inside:**
- `worker(void *arg)`: A minimal thread — atomically increments the `completed` counter and exits.
- `main()`: Calls `uthread_create()` 10,000 times in a loop. Prints progress every 2,000 threads. If any `uthread_create()` returns `-1` (out of memory), the test immediately fails.

**How to run:** `./tests/test_infinite_threads`

---

### `test_async_io.c` — Phase 12: Non-Blocking Sleep & I/O
**What it tests:** That threads can sleep for a duration without blocking the OS thread. While 4 threads sleep for 50ms, 100ms, 150ms, and 200ms, 4 other "busy" threads must run concurrently. Proves the scheduler doesn't freeze when threads are sleeping.

**Functions inside:**
- `sleeper(void *arg)`: Calls `uthread_sleep(ms)`. Records wall-clock time before and after and prints actual elapsed time to verify accuracy.
- `busy_worker(void *arg)`: Runs a tight loop of 1,000,000 iterations, doing real CPU work while the sleeper threads are blocked.
- `main()`: Enables tracing (`uthread_trace_enable()`), creates 4 sleepers and 4 busy workers, exits.

**How to run:** `./tests/test_async_io` (also generates `trace.json` for the Gantt viewer)

---

### `test_worksteal.c` — Phase 14: M:N Work Stealing
**What it tests:** That 100 CPU-bound threads are correctly distributed and completed across 4 Virtual Processors using the work-stealing algorithm.

**Functions inside:**
- `compute_worker(void *arg)`: Receives its ID. Runs a loop of 100,000 arithmetic operations (sum += i × id). Atomically increments `work_completed`. Prints progress at every 25 completions.
- `main()`: Calls `uthread_init_mn(4)` (4 VPs). Enables tracing. Creates 100 threads. Calls `uthread_mn_run()` to block until all VPs finish. Prints PASS if `work_completed == 100`.

**How to run:** `./tests/test_worksteal`

---

## 4. Shell Scripts

### `tests/run_mutex_test.sh`
**Purpose:** A convenience script that demonstrates the mutex race condition clearly in a single terminal run. Designed for presentations and demos.

**What it does step by step:**
1. Changes directory to the project root.
2. Runs `./tests/test_mutex nolock` once — shows the final counter is wrong (race condition). Typical output: `Final counter: 287432 (expected 400000) FAIL`
3. Runs `./tests/test_mutex lock` 5 times in a loop — shows the counter is a perfect 400,000 every single run, proving the mutex is bulletproof.

**How to run:** `bash tests/run_mutex_test.sh`

---

### `tests/run_semaphore_test.sh`
**Purpose:** A rigorous correctness verifier for the Producer-Consumer semaphore test. Runs the test 5 times and automatically parses the output to verify all 20 items were consumed exactly once every run.

**What it does step by step:**
1. Initializes `PASS=0` and `FAIL=0` counters.
2. Loops 5 times:
   - Captures the full output of `./tests/test_semaphore` into a variable.
   - Uses `grep` to extract lines containing "consumed item".
   - Uses `sed` and `sort -n` to extract just the item numbers.
   - Compares the sorted list against the expected sequence `0 1 2 3 ... 19`.
   - Prints `PASS` or `FAIL` for each run.
3. Prints a final score: `PASSED: N / 5   FAILED: M / 5`.

**How to run:** `bash tests/run_semaphore_test.sh`

---

## 5. Benchmarks Directory

### `benchmarks/benchmarks.c` — 1:N Benchmark Suite
**Purpose:** The original single-core benchmark. Measures three categories: context switch latency, thread throughput, and mutex contention — comparing `uthread` against native `pthreads`.

**Helper Functions:**
- `get_time_diff(start, end)`: Converts two `timespec` structs into a `double` seconds value.
- `wait_for_uthreads()`: Spinloops calling `uthread_yield()` until the global `active_threads` counter hits zero.

**Benchmark 1 — Context Switch Latency:**
- `b1_u_worker`: A `uthread` that calls `uthread_yield()` 50,000 times. Two of these run simultaneously, ping-ponging between each other for 100,000 total switches.
- `b1_p_worker`: A `pthread` equivalent that uses a `pthread_mutex` + `pthread_cond_wait` to alternate between two threads.

**Benchmark 2 — Thread Throughput:**
- `b2_u_worker` / `b2_p_worker`: Lightweight worker that runs a 1,000-iteration counter loop and exits. The benchmark spawns 200 (uthread) vs 200 (pthread) of these and measures total time.

**Benchmark 3 — Mutex Contention:**
- `b3_u_worker` / `b3_p_worker`: 8 threads that each lock/increment/unlock a shared counter 10,000 times (80,000 total lock acquisitions). Measures lock throughput under heavy contention.

**How to run:** `./benchmarks/benchmarks`

---

### `benchmarks/bench_worksteal.c` — M:N Work Stealing Benchmark
**Purpose:** Proves that the M:N work stealing engine achieves true multi-core parallelism on CPU-bound workloads. Uses `fork()` to isolate each benchmark in its own process since `uthread_exit()` terminates the process.

**Helper Functions:**
- `count_primes(int limit)`: Returns the count of prime numbers up to `limit`. CPU-intensive, used as the benchmark workload.
- `run_child(fn)`: Forks a child process, passes it a pipe file descriptor, collects the elapsed time from the pipe, and waits for the child.

**Benchmark Sections (each in a forked child):**
- `run_1n_bench(int pipe_fd)`: Single-core `uthread_init()`. Spawns 16 workers each counting primes up to 50,000. Writes elapsed time to pipe.
- `run_mn_bench(int pipe_fd, int num_vps)`: M:N `uthread_init_mn(4)`. Same 16 prime-counting workers, but spread across 4 VPs.
- **Pthread section** (inline in `main`): Directly uses `pthread_create()` for the same prime-counting task.
- **Context Switch Latency** (inline): 2 threads ping-ponging via `uthread_yield()` for 50,000 switches each.

**How to run:** `./benchmarks/bench_worksteal`

---

### `benchmarks/bench_demo.c` — Massive Concurrency Class Demo
**Purpose:** The "mic-drop" classroom presentation benchmark. Demonstrates that `uthread` handles massive concurrency (15,000 threads) dramatically faster than the OS kernel. Designed to be visually compelling with live progress updates.

**Functions:**
- `ut_worker(void *arg)`: A `uthread` that does nothing except atomically increment `done_count` and exit.
- `wait_for_ut()`: Polls `done_count` with `uthread_yield()`. Prints a live progress line every 3,000 completions so the class can watch it scroll.
- `pt_worker(void *arg)`: A `pthread` equivalent — returns immediately.
- `run_uthread_bench(int pipe_fd)`: Child process. Calls `uthread_init()`, spawns 15,000 `uthread`s with live spawn progress printed every 3,000, waits for all to finish, writes elapsed time to pipe.
- `run_pthread_bench(int pipe_fd)`: Child process. Allocates `pthread_t[15000]`, spawns all 15,000 with live progress, joins all with live progress, writes elapsed time to pipe.
- `run_child(fn)`: Forks a child process, gives it a pipe, reads the elapsed time result back.
- `main()`: Runs both benchmarks sequentially and prints a final banner: `"Your uthread library is Nx FASTER!"`.

**How to run:** `./benchmarks/bench_demo`

---

## 6. Tools Directory

### `tools/gantt_viewer.html`
**Role:** A standalone, single-file HTML + JavaScript web application. Visualizes the output of `uthread_dump_trace()` as an interactive Gantt chart.

**How it works:**
1. A user runs any test with `uthread_trace_enable()` active (e.g., `test_async_io` or `test_worksteal`).
2. The scheduler writes a `trace.json` file on exit via `uthread_dump_trace("trace.json")`.
3. The user opens `gantt_viewer.html` in a browser and drags the `trace.json` file onto the page.
4. The page parses the Chrome Trace Event Format JSON and renders a swimlane Gantt chart where each horizontal lane is one CPU core and colored blocks show which thread ran, yielded, slept, or stole work.

**What you can see in the viewer:**
- Thread creation events (instant markers).
- `RUN` blocks (how long each thread actually ran before yielding or being preempted).
- `YIELD`, `PREEMPT`, `BLOCK`, `SLEEP` events (transitions between states).
- `STEAL` events (work-stealing — which VP stole which thread from which other VP).
- Precise nanosecond timestamps on hover.

---

## 7. Build System (`Makefile`)

| Target | Command | Action |
|---|---|---|
| `all` | `make` | Compiles library, all tests, all benchmarks |
| `build` | `make build` | Compiles only `src/uthread.c` into `obj/uthread.o` |
| `test` | `make test` | Compiles all `tests/*.c` files into executables |
| `bench` | `make bench` | Compiles all `benchmarks/*.c` files into executables |
| `clean` | `make clean` | Deletes `obj/`, all test binaries, all benchmark binaries, and `trace.json` |

**Compiler Flags:** `-Wall -Wextra -pthread -g -std=c11`
- `-Wall -Wextra`: Maximum warnings (strict code quality).
- `-pthread`: Links the POSIX threads library (needed for M:N VPs).
- `-g`: Embeds debug symbols for GDB.
- `-std=c11`: Uses the C11 standard (required for `stdatomic.h`).
