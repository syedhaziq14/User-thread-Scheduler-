# uthread: Demo Presentation & Speaker Script

*This document is structured as a slide-by-slide guide for your OS course presentation. You can export this Markdown file to a PDF using VSCode extensions (like "Markdown PDF") or print it directly from your browser.*

---

## Slide 1: Title Slide
**Title:** `uthread`: High-Performance User-Space Thread Scheduler  
**Speaker Notes:**  
"Hello everyone. Today I'm going to present `uthread`, a lightweight, user-space thread scheduling library that I built from scratch in C. This project simulates the core engine behind modern concurrency models like Go's Goroutines and Java's Project Loom."

---

## Slide 2: The Problem (Why do we need this?)
**Visuals/Bullet Points:**
* Native OS threads (`pthreads`) are heavy and expensive.
* Kernel traps: Context switching requires jumping to Ring 0 (Kernel Mode).
* Memory Bloat: Default OS thread stacks are huge (e.g., 8MB).

**Speaker Notes:**  
"What problem does this solve? Imagine you are building a web server like Discord, handling 10,000 concurrent user connections. If you spawn one native OS thread per user, the kernel has to allocate 8MB of RAM for each thread. 10,000 threads * 8MB = 80 Gigabytes of RAM just for idle stacks! Furthermore, the CPU wastes massive amounts of time constantly trapping into the OS kernel just to switch between them. The OS kernel becomes the bottleneck."

---

## Slide 3: The Solution (User-Space Threads)
**Visuals/Bullet Points:**
* Stay in User Space (Ring 3).
* Cooperative & Preemptive Scheduling without kernel intervention.
* Sub-microsecond context switches.

**Speaker Notes:**  
"The solution is user-space threading, often called 'Green Threads'. Instead of asking the OS to manage our threads, we spawn a *single* OS thread, and write our own mini-OS inside of it to manage thousands of tiny 'virtual' threads. Because we never trap into the kernel, our context switches are just simple pointer swaps. We allocate tiny 64KB stacks instead of 8MB. This is exactly how the Go programming language achieves massive scalability."

---

## Slide 4: Real-Time Architecture & Preemption
**Visuals/Bullet Points:**
* **`ucontext_t`**: POSIX API to save CPU registers/state.
* **Pluggable Schedulers**: Round-Robin and Priority Queues.
* **Preemption**: Hardware Timers (`SIGALRM`).

**Speaker Notes:**  
"Under the hood, `uthread` uses the `ucontext_t` library to save and restore CPU registers. I built a pluggable scheduler that allows hot-swapping between Round-Robin and strict Priority queues. But what happens if a thread has an infinite loop? I implemented true Preemption. I programmed a hardware timer to fire a `SIGALRM` every 10 milliseconds. This interrupts the CPU, pauses the running thread, and forces a context switch, completely preventing any single thread from freezing the system."

---

## Slide 5: Advanced Synchronization (No Spinlocks!)
**Visuals/Bullet Points:**
* Built Mutexes and Counting Semaphores.
* The danger of Spinlocks (wasted CPU cycles).
* `BLOCKED` Thread State.

**Speaker Notes:**  
"For synchronization, I built Mutexes and Semaphores. A naive implementation would use 'spinlocks'—where a waiting thread constantly loops `while(locked);`. That wastes CPU cycles. Instead, my mutex modifies the thread's state to `BLOCKED` and physically removes it from the Ready Queue. The thread consumes zero CPU time until the lock holder explicitly wakes it up."

---

## Slide 6: The Peterson's Solution Paradox
**Visuals/Bullet Points:**
* Textbook Peterson's Solution works in theory (Sequential Consistency).
* Fails in reality (Compiler Reordering, Out-of-Order CPU Execution).

**Speaker Notes:**  
"As an academic experiment, I implemented the classic Peterson's Solution. While it is mathematically proven to work in textbooks, my demo proves it fails on modern hardware. Because modern CPUs dynamically reorder instructions to run faster, and compilers optimize code aggressively, memory reads and writes happen out of order. This proves why raw shared-memory flags are unsafe, and why we must rely on robust scheduler-level locking."

---

## Slide 7: Go-Style Message Channels
**Visuals/Bullet Points:**
* Inter-thread communication.
* Built using Semaphores + Mutexes.
* 3-Stage Pipeline Demo.

**Speaker Notes:**  
"To show the power of these primitives, I built a Message Passing Channel system, heavily inspired by Go's Channels. Threads can safely pass data through a bounded circular buffer. If the buffer is full, the sender goes to sleep. If it's empty, the receiver goes to sleep. I verified this with a 3-stage concurrent pipeline where data flows seamlessly without deadlocking."

---

## Slide 8: Real Benchmark Data (uthread vs pthread)
**Visuals/Bullet Points:**
* **Throughput (Spawn 1000 threads):** `uthread` (0.01s) vs `pthread` (0.24s). -> **16x Faster**
* **Latency (100k Context Switches):** `uthread` (0.59s) vs `pthread` (4.58s). -> **7.7x Faster**
* **Limitation:** Native `pthreads` win on heavy multi-core contention.

**Speaker Notes:**  
"Finally, I benchmarked `uthread` against native Linux `pthreads`. The results are staggering. `uthread` can spawn 1,000 threads and tear them down 16 times faster than the OS. Our context switch latency is nearly 8 times faster. However, I didn't fabricate data to look perfect: `pthreads` heavily outperformed us on Mutex Contention because native OS threads can run on multiple physical CPU cores simultaneously, while my simulation is currently bound to a single core."

---

## Slide 9: Conclusion & Q&A
**Visuals/Bullet Points:**
* Built a functional mini-OS inside a single process.
* Deep understanding of memory models, context switching, and concurrency.

**Speaker Notes:**  
"Building `uthread` was essentially building a mini Operating System scheduler from scratch. It bridges the gap between theoretical OS concepts and real-world runtime environments. Thank you, I will now take any questions."

---

### How to Convert this to PDF:
1. If you are using **VSCode**, install the extension **"Markdown PDF"**. Right-click anywhere in this file and select "Markdown PDF: Export (pdf)".
2. Alternatively, open this Markdown file in your web browser (using a Markdown viewer extension) or upload it to GitHub, press `Ctrl+P` (Print), and select **"Save as PDF"**.
