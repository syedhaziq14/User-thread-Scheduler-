# Peterson's Solution: Theoretical Correctness vs. Real-world Reality

## 1. How the Algorithm Works
Peterson's algorithm achieves two-thread mutual exclusion using two shared variables: a `flag` array indicating a thread's desire to enter the critical section, and a `turn` variable resolving simultaneous requests. 
When a thread wants to enter, it sets its `flag` to `true` and generously sets `turn` to the *other* thread's ID. It then spins in a `while` loop as long as the other thread also wants to enter (`flag[other] == true`) AND it is the other thread's turn (`turn == other`). 
If both threads try to enter simultaneously, the last thread to write to `turn` will overwrite it. The first thread to write to `turn` proceeds, while the second thread spins, cleanly preventing race conditions. Upon exiting the critical section, the thread sets its `flag` to `false`, releasing the lock.

## 2. Correctness under Sequential Consistency
In the theoretical Sequential Consistency model, every operation executes in the exact order written in the source code, and memory operations are immediately globally visible to all processors in a single order. Under this idealized model, Peterson's algorithm is mathematically proven to guarantee mutual exclusion (both threads cannot simultaneously see `flag == false` or hold the `turn`), progress (a thread will eventually enter), and bounded waiting (a thread waits at most one turn).

## 3. Why Real CPUs and Compilers Break It
On modern hardware and compilers, sequential consistency is a myth due to two primary factors:
- **Compiler Reordering**: Aggressive compilers (like `gcc -O3`) analyze code and reorder instructions to optimize pipeline usage. The compiler might hoist the `while` loop condition check *before* the writes to `flag` and `turn`, or cache the shared variables in local registers, instantly breaking the algorithm's synchronization logic.
- **Out-of-Order Execution**: Modern CPUs (like x86 and ARM) dynamically reorder instructions at runtime to maximize instruction throughput. A CPU might buffer the writes to `flag` and `turn` in a store buffer while speculatively executing the read inside the `while` loop. If both CPUs delay their writes but execute their reads, both will see the old values in memory and enter the critical section simultaneously.

Without explicit memory barriers (fences) and `atomic` instructions to force ordering, Peterson's algorithm simply fails on modern architectures.

## 4. How our `uthread_mutex_t` Avoids This
Our `uthread_mutex_t` implementation (from Phase 3) does not suffer from these issues because it avoids spinning on raw shared flags entirely. Instead, it relies on the scheduler's robust synchronization. When a thread fails to acquire a lock via `uthread_mutex_lock`, it explicitly modifies its own state to `BLOCKED` and removes itself from the ready queue via an OS-level context switch (`swapcontext`). The thread only executes again when `uthread_mutex_unlock` intentionally places it back in the ready queue. This scheduler-enforced approach guarantees absolute mutual exclusion without relying on fragile memory consistency assumptions.
