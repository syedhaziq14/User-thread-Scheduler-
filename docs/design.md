# uthread Design Document

## 1. Why `ucontext_t`?
The core mechanism of any thread scheduler is the ability to save the CPU's current execution state (registers, stack pointer, program counter) and restore a different one. 
We chose the POSIX `ucontext_t` family (`getcontext`, `makecontext`, `swapcontext`) because it provides a clean, standardized, and hardware-agnostic API for manipulating context in user-space.

Alternatives included:
- **`setjmp` / `longjmp`**: While widely available, they are designed for non-local gotos and do not provide a safe or portable way to swap stack pointers or initialize a brand new execution stack for a function.
- **Inline Assembly**: Writing custom assembly to push/pop registers (like `switch_to` in the Linux kernel) is highly performant but extremely unportable, requiring different implementations for x86_64, ARM, etc. `ucontext.h` abstracts these ABI-specific details away perfectly.

## 2. Thread Control Block (TCB)
The `tcb_t` struct serves as the brain for each thread. Key fields include:
- `thread_id`: A unique integer identifying the thread.
- `context` (`ucontext_t`): Stores the CPU state when the thread is swapped out.
- `stack`: A heap-allocated memory region (default 64KB) serving as the thread's execution stack.
- `state`: The thread's lifecycle stage (`READY`, `RUNNING`, `BLOCKED`, `FINISHED`).
- `priority` & `creation_order`: Used by the pluggable scheduler to determine execution order. `creation_order` serves as a tie-breaker to prevent starvation among threads of equal priority.

## 3. Preemption and Signal Safety
To prevent rogue CPU-bound threads from monopolizing the system, `uthread` utilizes a preemptive timer (`setitimer` with `ITIMER_REAL` and `SIGALRM`). Every 10ms, a signal handler forces a context switch via `uthread_yield()`.

**The Danger of Scheduler Re-entrancy**:
If a timer interrupt fires while the scheduler is already actively modifying shared state (such as updating the `ready_queue` array during a mutex unlock), the signal handler would forcibly re-enter the scheduler. The scheduler would then read partially-updated, corrupted data, leading to a segmentation fault.

**The Solution**:
We utilize `sigprocmask(SIG_BLOCK, ...)` to mask out `SIGALRM` before entering any critical scheduler section (`yield`, `create`, `lock`, `unlock`). Signals are only unblocked when control is returned to user-space code.

## 4. Peterson's Solution vs. Scheduler-level Synchronization
In Phase 6, we implemented Peterson’s Solution to demonstrate theoretical, purely mathematical mutual exclusion. 

While Peterson's algorithm is provably correct under the "Sequential Consistency" memory model taught in OS textbooks, we verified that it **fails completely on modern hardware and compilers**. Aggressive compilers (like `gcc -O3`) reorder instructions, and modern CPUs (like x86) buffer memory stores, violating the core assumptions of the algorithm unless costly hardware memory fences are used.

Because of this, our actual `uthread_mutex_t` deliberately ignores spinning on shared memory flags. Instead, it relies on the robust synchronization capabilities of the scheduler itself. When a thread cannot acquire a lock, it explicitly alters its `state` to `BLOCKED` and removes itself from the ready queue via `swapcontext`. Absolute mutual exclusion is guaranteed because the OS-level scheduling abstraction physically prevents the waiting thread from executing any code until the lock holder wakes it up.
