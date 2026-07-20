#include "uthread.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define NSEC_PER_SEC 1000000000L

double get_time_diff(struct timespec start, struct timespec end) {
    double s = start.tv_sec + (double)start.tv_nsec / NSEC_PER_SEC;
    double e = end.tv_sec + (double)end.tv_nsec / NSEC_PER_SEC;
    return e - s;
}

volatile int active_threads = 0;

void wait_for_uthreads() {
    while (active_threads > 0) {
        uthread_yield();
    }
}

// ==========================================
// Benchmark 1: Context Switch Latency
// ==========================================
int b1_ucount = 0;
void b1_u_worker(void *arg) {
    (void)arg;
    for (int i = 0; i < 50000; i++) {
        b1_ucount++;
        uthread_yield();
    }
    __sync_fetch_and_sub(&active_threads, 1);
}

pthread_mutex_t b1_p_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t b1_p_cond = PTHREAD_COND_INITIALIZER;
int b1_p_turn = 0;
int b1_p_count = 0;

void* b1_p_worker(void *arg) {
    int id = *(int*)arg;
    for (int i = 0; i < 50000; i++) {
        pthread_mutex_lock(&b1_p_mutex);
        while (b1_p_turn != id) {
            pthread_cond_wait(&b1_p_cond, &b1_p_mutex);
        }
        b1_p_count++;
        b1_p_turn = 1 - id;
        pthread_cond_signal(&b1_p_cond);
        pthread_mutex_unlock(&b1_p_mutex);
    }
    return NULL;
}

// ==========================================
// Benchmark 2: Throughput
// ==========================================
void b2_u_worker(void *arg) {
    (void)arg;
    volatile int local_count = 0;
    for (int i = 0; i < 1000; i++) {
        local_count++;
    }
    __sync_fetch_and_sub(&active_threads, 1);
}

void* b2_p_worker(void *arg) {
    (void)arg;
    volatile int local_count = 0;
    for (int i = 0; i < 1000; i++) {
        local_count++;
    }
    return NULL;
}

// ==========================================
// Benchmark 3: Mutex Contention
// ==========================================
uthread_mutex_t b3_u_mutex;
int b3_u_counter = 0;
void b3_u_worker(void *arg) {
    (void)arg;
    for (int i = 0; i < 10000; i++) {
        uthread_mutex_lock(&b3_u_mutex);
        b3_u_counter++;
        uthread_mutex_unlock(&b3_u_mutex);
    }
    __sync_fetch_and_sub(&active_threads, 1);
}

pthread_mutex_t b3_p_mutex = PTHREAD_MUTEX_INITIALIZER;
int b3_p_counter = 0;
void* b3_p_worker(void *arg) {
    (void)arg;
    for (int i = 0; i < 10000; i++) {
        pthread_mutex_lock(&b3_p_mutex);
        b3_p_counter++;
        pthread_mutex_unlock(&b3_p_mutex);
    }
    return NULL;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    struct timespec start, end;
    double t_u1, t_p1, t_u2, t_p2, t_u3, t_p3;
    
    printf("Initializing uthread library...\n");
    uthread_init();

    // ------------------------------------------
    // Run Benchmark 1
    // ------------------------------------------
    printf("Running Benchmark 1: Context Switch Latency...\n");
    
    // uthread
    active_threads = 2;
    clock_gettime(CLOCK_MONOTONIC, &start);
    uthread_create(b1_u_worker, NULL);
    uthread_create(b1_u_worker, NULL);
    wait_for_uthreads();
    clock_gettime(CLOCK_MONOTONIC, &end);
    t_u1 = get_time_diff(start, end);

    // pthread
    pthread_t p1_0, p1_1;
    int id0 = 0, id1 = 1;
    clock_gettime(CLOCK_MONOTONIC, &start);
    pthread_create(&p1_0, NULL, b1_p_worker, &id0);
    pthread_create(&p1_1, NULL, b1_p_worker, &id1);
    pthread_join(p1_0, NULL);
    pthread_join(p1_1, NULL);
    clock_gettime(CLOCK_MONOTONIC, &end);
    t_p1 = get_time_diff(start, end);

    // ------------------------------------------
    // Run Benchmark 2
    // ------------------------------------------
    printf("Running Benchmark 2: Throughput under load...\n");
    int NUM_THREADS = 200;
    
    // uthread
    active_threads = NUM_THREADS;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < NUM_THREADS; i++) {
        if (uthread_create(b2_u_worker, NULL) < 0) {
            printf("uthread_create failed at index %d\n", i);
            exit(1);
        }
    }
    wait_for_uthreads();
    clock_gettime(CLOCK_MONOTONIC, &end);
    t_u2 = get_time_diff(start, end);

    // pthread
    pthread_t *pt_arr = malloc(sizeof(pthread_t) * NUM_THREADS);
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < NUM_THREADS; i++) {
        if (pthread_create(&pt_arr[i], NULL, b2_p_worker, NULL) != 0) {
            printf("pthread_create failed at index %d\n", i);
            exit(1);
        }
    }
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(pt_arr[i], NULL);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    t_p2 = get_time_diff(start, end);
    free(pt_arr);

    // ------------------------------------------
    // Run Benchmark 3
    // ------------------------------------------
    printf("Running Benchmark 3: Mutex Contention...\n");
    int NUM_MUTEX_THREADS = 8;
    
    // uthread
    uthread_mutex_init(&b3_u_mutex);
    active_threads = NUM_MUTEX_THREADS;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < NUM_MUTEX_THREADS; i++) {
        uthread_create(b3_u_worker, NULL);
    }
    wait_for_uthreads();
    clock_gettime(CLOCK_MONOTONIC, &end);
    t_u3 = get_time_diff(start, end);

    // pthread
    pthread_t *pt_arr3 = malloc(sizeof(pthread_t) * NUM_MUTEX_THREADS);
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < NUM_MUTEX_THREADS; i++) {
        pthread_create(&pt_arr3[i], NULL, b3_p_worker, NULL);
    }
    for (int i = 0; i < NUM_MUTEX_THREADS; i++) {
        pthread_join(pt_arr3[i], NULL);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    t_p3 = get_time_diff(start, end);
    free(pt_arr3);

    // ------------------------------------------
    // Output Results
    // ------------------------------------------
    printf("\n=========================================================================\n");
    printf("%-15s | %-30s | %-12s | %-10s\n", "System", "Benchmark", "Time (s)", "Iterations");
    printf("-------------------------------------------------------------------------\n");
    printf("%-15s | %-30s | %-12.6f | %-10d\n", "uthread", "Context Switch Latency", t_u1, 100000);
    printf("%-15s | %-30s | %-12.6f | %-10d\n", "pthread", "Context Switch Latency", t_p1, 100000);
    printf("%-15s | %-30s | %-12.6f | %-10d\n", "uthread", "Throughput (1000 threads)", t_u2, 1000);
    printf("%-15s | %-30s | %-12.6f | %-10d\n", "pthread", "Throughput (1000 threads)", t_p2, 1000);
    printf("%-15s | %-30s | %-12.6f | %-10d\n", "uthread", "Mutex Contention (8 threads)", t_u3, 80000);
    printf("%-15s | %-30s | %-12.6f | %-10d\n", "pthread", "Mutex Contention (8 threads)", t_p3, 80000);
    printf("=========================================================================\n\n");

    FILE *f = fopen("results.csv", "w");
    if (f) {
        fprintf(f, "System,Benchmark,Time_s,Iterations\n");
        fprintf(f, "uthread,Context Switch Latency,%f,100000\n", t_u1);
        fprintf(f, "pthread,Context Switch Latency,%f,100000\n", t_p1);
        fprintf(f, "uthread,Throughput,%f,1000\n", t_u2);
        fprintf(f, "pthread,Throughput,%f,1000\n", t_p2);
        fprintf(f, "uthread,Mutex Contention,%f,80000\n", t_u3);
        fprintf(f, "pthread,Mutex Contention,%f,80000\n", t_p3);
        fclose(f);
        printf("Results exported to benchmarks/results.csv\n");
    }

    // exit program explicitly since main thread bypasses uthread_exit
    exit(0);
}
