/*
 * Thread-safe malloc test suite (Fixed for 4-byte alignment)
 * Compile: gcc -o malloc_test malloc_test.c -lpthread
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <assert.h>

/* Configuration */
#define NUM_THREADS 8
#define ITERATIONS_PER_THREAD 1000
#define MAX_ALLOC_SIZE 4096
#define SMALL_ALLOC_SIZE 64
#define STRESS_TEST_DURATION 5  /* seconds */

/* Alignment to check (4 bytes for 32-bit systems, 8 bytes for 64-bit) */
#if defined(__LP64__) || defined(_WIN64)
    #define ALIGNMENT_BYTES 8
    #define ALIGNMENT_MASK 0x7
#else
    #define ALIGNMENT_BYTES 4
    #define ALIGNMENT_MASK 0x3
#endif

/* Test statistics */
typedef struct {
    unsigned long allocations;
    unsigned long frees;
    unsigned long reallocs;
    unsigned long errors;
    unsigned long corruptions;
    unsigned long misalignments;
} thread_stats_t;

thread_stats_t stats[NUM_THREADS];
volatile int stop_flag = 0;

/* Simple random number generator (thread-safe with seed) */
static unsigned int rand_r_simple(unsigned int *seed) {
    *seed = *seed * 1103515245 + 12345;
    return (*seed / 65536) % 32768;
}

/* Test 1: Basic allocation and deallocation */
void* test_basic_alloc_free(void* arg) {
    int thread_id = *(int*)arg;
    unsigned int seed = thread_id + time(NULL);
    
    printf("[Thread %d] Starting basic alloc/free test\n", thread_id);
    
    for (int i = 0; i < ITERATIONS_PER_THREAD; i++) {
        size_t size = (rand_r_simple(&seed) % MAX_ALLOC_SIZE) + 1;
        void* ptr = malloc(size);
        
        if (ptr == NULL) {
            stats[thread_id].errors++;
            continue;
        }
        
        stats[thread_id].allocations++;
        
        /* Check alignment */
        if (((unsigned long)ptr & ALIGNMENT_MASK) != 0) {
            stats[thread_id].misalignments++;
        }
        
        /* Write pattern to memory */
        memset(ptr, (i & 0xFF), size);
        
        /* Verify pattern */
        unsigned char* check = (unsigned char*)ptr;
        for (size_t j = 0; j < size; j++) {
            if (check[j] != (i & 0xFF)) {
                stats[thread_id].corruptions++;
                break;
            }
        }
        
        free(ptr);
        stats[thread_id].frees++;
        
        /* Occasional yield to increase contention */
        if (i % 100 == 0) {
            usleep(1);
        }
    }
    
    printf("[Thread %d] Completed basic test\n", thread_id);
    return NULL;
}

/* Test 2: Mixed size allocations with delayed frees */
void* test_mixed_delayed_free(void* arg) {
    int thread_id = *(int*)arg;
    unsigned int seed = thread_id * 100 + time(NULL);
    void* ptrs[100];
    size_t sizes[100];
    int alloc_count = 0;
    
    printf("[Thread %d] Starting mixed delayed free test\n", thread_id);
    
    for (int i = 0; i < ITERATIONS_PER_THREAD; i++) {
        /* Randomly allocate or free */
        if (alloc_count < 100 && (alloc_count == 0 || rand_r_simple(&seed) % 2 == 0)) {
            /* Allocate */
            size_t size = (rand_r_simple(&seed) % MAX_ALLOC_SIZE) + 1;
            void* ptr = malloc(size);
            
            if (ptr != NULL) {
                ptrs[alloc_count] = ptr;
                sizes[alloc_count] = size;
                memset(ptr, (alloc_count & 0xFF), size);
                alloc_count++;
                stats[thread_id].allocations++;
                
                /* Check alignment */
                if (((unsigned long)ptr & ALIGNMENT_MASK) != 0) {
                    stats[thread_id].misalignments++;
                }
            } else {
                stats[thread_id].errors++;
            }
        } else if (alloc_count > 0) {
            /* Free a random allocation */
            int idx = rand_r_simple(&seed) % alloc_count;
            
            /* Verify before freeing */
            unsigned char* check = (unsigned char*)ptrs[idx];
            for (size_t j = 0; j < sizes[idx]; j++) {
                if (check[j] != (idx & 0xFF)) {
                    stats[thread_id].corruptions++;
                    break;
                }
            }
            
            free(ptrs[idx]);
            stats[thread_id].frees++;
            
            /* Replace with last element */
            ptrs[idx] = ptrs[alloc_count - 1];
            sizes[idx] = sizes[alloc_count - 1];
            alloc_count--;
        }
    }
    
    /* Free remaining allocations */
    for (int i = 0; i < alloc_count; i++) {
        free(ptrs[i]);
        stats[thread_id].frees++;
    }
    
    printf("[Thread %d] Completed mixed delayed free test\n", thread_id);
    return NULL;
}

/* Test 3: Stress test with many small allocations */
void* test_small_alloc_stress(void* arg) {
    int thread_id = *(int*)arg;
    unsigned int seed = thread_id * 200 + time(NULL);
    
    printf("[Thread %d] Starting small allocation stress test\n", thread_id);
    
    time_t start = time(NULL);
    while (time(NULL) - start < STRESS_TEST_DURATION && !stop_flag) {
        size_t size = (rand_r_simple(&seed) % SMALL_ALLOC_SIZE) + 1;
        void* ptr = malloc(size);
        
        if (ptr != NULL) {
            stats[thread_id].allocations++;
            memset(ptr, 0xAA, size);
            free(ptr);
            stats[thread_id].frees++;
        } else {
            stats[thread_id].errors++;
        }
    }
    
    printf("[Thread %d] Completed stress test\n", thread_id);
    return NULL;
}

/* Test 4: NULL pointer and edge cases */
void* test_edge_cases(void* arg) {
    int thread_id = *(int*)arg;
    
    printf("[Thread %d] Starting edge case test\n", thread_id);
    
    for (int i = 0; i < ITERATIONS_PER_THREAD / 10; i++) {
        /* Test free(NULL) - should not crash */
        free(NULL);
        stats[thread_id].frees++;
        
        /* Test zero-size allocation (implementation defined) */
        void* ptr = malloc(0);
        if (ptr != NULL) {
            stats[thread_id].allocations++;
            free(ptr);
            stats[thread_id].frees++;
        }
        
        /* Test various small sizes */
        for (size_t size = 1; size <= 64; size++) {
            ptr = malloc(size);
            if (ptr != NULL) {
                stats[thread_id].allocations++;
                
                /* Check alignment */
                if (((unsigned long)ptr & ALIGNMENT_MASK) != 0) {
                    stats[thread_id].misalignments++;
                }
                
                /* Write and verify */
                memset(ptr, size & 0xFF, size);
                unsigned char* check = (unsigned char*)ptr;
                for (size_t j = 0; j < size; j++) {
                    if (check[j] != (size & 0xFF)) {
                        stats[thread_id].corruptions++;
                        break;
                    }
                }
                
                free(ptr);
                stats[thread_id].frees++;
            }
        }
    }
    
    printf("[Thread %d] Completed edge case test\n", thread_id);
    return NULL;
}

/* Test 5: Fragmentation test */
void* test_fragmentation(void* arg) {
    int thread_id = *(int*)arg;
    unsigned int seed = thread_id * 300 + time(NULL);
    void* ptrs[50];
    
    printf("[Thread %d] Starting fragmentation test\n", thread_id);
    
    for (int iter = 0; iter < ITERATIONS_PER_THREAD / 20; iter++) {
        /* Allocate blocks of varying sizes */
        for (int i = 0; i < 50; i++) {
            size_t size = ((rand_r_simple(&seed) % 10) + 1) * 128;
            ptrs[i] = malloc(size);
            if (ptrs[i] != NULL) {
                stats[thread_id].allocations++;
                memset(ptrs[i], i, size);
            } else {
                stats[thread_id].errors++;
                ptrs[i] = NULL;
            }
        }
        
        /* Free every other block (create fragmentation) */
        for (int i = 0; i < 50; i += 2) {
            if (ptrs[i] != NULL) {
                free(ptrs[i]);
                stats[thread_id].frees++;
                ptrs[i] = NULL;
            }
        }
        
        /* Try to allocate in the gaps */
        for (int i = 0; i < 25; i++) {
            size_t size = (rand_r_simple(&seed) % 256) + 1;
            void* ptr = malloc(size);
            if (ptr != NULL) {
                stats[thread_id].allocations++;
                free(ptr);
                stats[thread_id].frees++;
            }
        }
        
        /* Free remaining blocks */
        for (int i = 1; i < 50; i += 2) {
            if (ptrs[i] != NULL) {
                free(ptrs[i]);
                stats[thread_id].frees++;
            }
        }
    }
    
    printf("[Thread %d] Completed fragmentation test\n", thread_id);
    return NULL;
}

/* Test 6: Double-free detection test (should not crash) */
void* test_double_free_protection(void* arg) {
    int thread_id = *(int*)arg;
    unsigned int seed = thread_id * 400 + time(NULL);
    
    printf("[Thread %d] Starting double-free protection test\n", thread_id);
    
    for (int i = 0; i < ITERATIONS_PER_THREAD / 20; i++) {
        size_t size = (rand_r_simple(&seed) % 1024) + 1;
        void* ptr = malloc(size);
        
        if (ptr != NULL) {
            stats[thread_id].allocations++;
            memset(ptr, 0x55, size);
            
            /* Free once (valid) */
            free(ptr);
            stats[thread_id].frees++;
            
            /* Attempt double-free (should be caught by valid check) */
            free(ptr);
            stats[thread_id].frees++;
        }
    }
    
    printf("[Thread %d] Completed double-free protection test\n", thread_id);
    return NULL;
}

/* Print statistics */
void print_stats(const char* test_name, double elapsed_time) {
    unsigned long total_allocs = 0;
    unsigned long total_frees = 0;
    unsigned long total_errors = 0;
    unsigned long total_corruptions = 0;
    unsigned long total_misalignments = 0;
    
    printf("\n=== %s Results ===\n", test_name);
    printf("%-10s %10s %10s %10s %10s %10s\n", 
           "Thread", "Allocs", "Frees", "Errors", "Corrupt", "Misalign");
    printf("--------------------------------------------------------------------\n");
    
    for (int i = 0; i < NUM_THREADS; i++) {
        printf("%-10d %10lu %10lu %10lu %10lu %10lu\n",
               i, stats[i].allocations, stats[i].frees,
               stats[i].errors, stats[i].corruptions, stats[i].misalignments);
        
        total_allocs += stats[i].allocations;
        total_frees += stats[i].frees;
        total_errors += stats[i].errors;
        total_corruptions += stats[i].corruptions;
        total_misalignments += stats[i].misalignments;
    }
    
    printf("--------------------------------------------------------------------\n");
    printf("%-10s %10lu %10lu %10lu %10lu %10lu\n",
           "TOTAL", total_allocs, total_frees, total_errors, 
           total_corruptions, total_misalignments);
    printf("\nElapsed time: %.2f seconds\n", elapsed_time);
    printf("Operations/sec: %.0f\n", (total_allocs + total_frees) / elapsed_time);
    printf("Alignment: %d bytes (mask: 0x%lx)\n", ALIGNMENT_BYTES, ALIGNMENT_MASK);
    
    if (total_corruptions > 0) {
        printf("\n*** FAILED: Memory corruptions detected! ***\n");
    } else if (total_misalignments > 0) {
        printf("\n*** WARNING: %lu misaligned pointers detected! ***\n", total_misalignments);
    } else if (total_errors > 0) {
        printf("\n*** WARNING: %lu allocation errors occurred ***\n", total_errors);
    } else {
        printf("\n*** TEST PASSED: No corruptions or misalignments! ***\n");
    }
}

/* Run a test with specified function */
void run_test(const char* name, void* (*test_func)(void*)) {
    pthread_t threads[NUM_THREADS];
    int thread_ids[NUM_THREADS];
    struct timespec start, end;
    
    /* Reset statistics */
    memset(stats, 0, sizeof(stats));
    stop_flag = 0;
    
    printf("\n========================================\n");
    printf("Running: %s\n", name);
    printf("========================================\n");
    
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    /* Create threads */
    for (int i = 0; i < NUM_THREADS; i++) {
        thread_ids[i] = i;
        if (pthread_create(&threads[i], NULL, test_func, &thread_ids[i]) != 0) {
            perror("pthread_create");
            exit(1);
        }
    }
    
    /* Wait for completion */
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (end.tv_sec - start.tv_sec) + 
                     (end.tv_nsec - start.tv_nsec) / 1e9;
    
    print_stats(name, elapsed);
}

int main(int argc, char* argv[]) {
    printf("Thread-safe malloc test suite\n");
    printf("Threads: %d\n", NUM_THREADS);
    printf("Iterations per thread: %d\n", ITERATIONS_PER_THREAD);
    printf("Expected alignment: %d bytes\n\n", ALIGNMENT_BYTES);
    
    /* Run all tests */
    run_test("Test 1: Basic Alloc/Free", test_basic_alloc_free);
    run_test("Test 2: Mixed Delayed Free", test_mixed_delayed_free);
    run_test("Test 3: Small Alloc Stress", test_small_alloc_stress);
    run_test("Test 4: Edge Cases", test_edge_cases);
    run_test("Test 5: Fragmentation", test_fragmentation);
    run_test("Test 6: Double-Free Protection", test_double_free_protection);
    
    printf("\n========================================\n");
    printf("All tests completed!\n");
    printf("========================================\n");
    
    return 0;
}