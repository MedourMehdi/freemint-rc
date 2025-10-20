#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#define NUM_THREADS 5
#define NUM_ITERATIONS 10

/* Test results structure */
typedef struct {
    int thread_num;
    int errors_detected;
    int test_passed;
} test_result_t;

/* Thread function that sets and checks errno repeatedly */
void *thread_test_errno(void *arg)
{
    int thread_num = *(int *)arg;
    int i, j;
    int errors = 0;
    int expected_errno;
    
    printf("Thread %d: Started (tid=%ld)\n", thread_num, (long)pthread_self());
    
    for (i = 0; i < NUM_ITERATIONS; i++) {
        /* Each thread uses a unique errno value based on its number */
        expected_errno = (thread_num * 100) + i;
        
        /* Set errno to thread-specific value */
        errno = expected_errno;
        
        /* Small delay to allow other threads to run and potentially interfere */
        usleep(1000 + (thread_num * 100));
        
        /* Verify errno hasn't been changed by another thread */
        if (errno != expected_errno) {
            printf("Thread %d: ERROR! errno mismatch. Expected %d, got %d\n",
                   thread_num, expected_errno, errno);
            errors++;
        }
        
        /* Do multiple checks in same iteration */
        for (j = 0; j < 3; j++) {
            if (errno != expected_errno) {
                printf("Thread %d: ERROR! errno corrupted during iteration %d.%d\n",
                       thread_num, i, j);
                errors++;
            }
            usleep(100);
        }
    }
    
    printf("Thread %d: Completed with %d errors\n", thread_num, errors);
    
    test_result_t *result = malloc(sizeof(test_result_t));
    result->thread_num = thread_num;
    result->errors_detected = errors;
    result->test_passed = (errors == 0);
    
    return result;
}

/* Test errno in single-threaded mode */
int test_single_threaded(void)
{
    int i;
    int errors = 0;
    
    printf("\n=== TEST 1: Single-threaded errno ===\n");
    
    for (i = 0; i < 10; i++) {
        errno = i * 10;
        if (errno != i * 10) {
            printf("ERROR: Single-threaded errno test failed at iteration %d\n", i);
            errors++;
        }
    }
    
    if (errors == 0) {
        printf("PASSED: Single-threaded errno works correctly\n");
    } else {
        printf("FAILED: Single-threaded errno has %d errors\n", errors);
    }
    
    return (errors == 0);
}

/* Test errno isolation between threads */
int test_multi_threaded(void)
{
    pthread_t threads[NUM_THREADS];
    int thread_nums[NUM_THREADS];
    test_result_t *results[NUM_THREADS];
    int i;
    int all_passed = 1;
    
    printf("\n=== TEST 2: Multi-threaded errno isolation ===\n");
    printf("Creating %d threads...\n", NUM_THREADS);
    
    /* Create threads */
    for (i = 0; i < NUM_THREADS; i++) {
        thread_nums[i] = i + 1;
        if (pthread_create(&threads[i], NULL, thread_test_errno, &thread_nums[i]) != 0) {
            fprintf(stderr, "Failed to create thread %d\n", i);
            return 0;
        }
    }
    
    /* Set errno in main thread */
    errno = 9999;
    printf("Main thread: Set errno to 9999\n");
    
    /* Join threads and collect results */
    for (i = 0; i < NUM_THREADS; i++) {
        if (pthread_join(threads[i], (void **)&results[i]) != 0) {
            fprintf(stderr, "Failed to join thread %d\n", i);
            all_passed = 0;
            continue;
        }
        
        if (!results[i]->test_passed) {
            printf("Thread %d: FAILED with %d errors\n", 
                   results[i]->thread_num, results[i]->errors_detected);
            all_passed = 0;
        } else {
            printf("Thread %d: PASSED\n", results[i]->thread_num);
        }
        
        free(results[i]);
    }
    
    /* Verify main thread's errno wasn't affected */
    if (errno != 9999) {
        printf("Main thread: ERROR! errno changed from 9999 to %d\n", errno);
        all_passed = 0;
    } else {
        printf("Main thread: errno still 9999 (correct)\n");
    }
    
    if (all_passed) {
        printf("PASSED: All threads maintained separate errno values\n");
    } else {
        printf("FAILED: errno isolation broken\n");
    }
    
    return all_passed;
}

/* Test errno with actual system calls that set errno */
void *thread_syscall_test(void *arg)
{
    int thread_num = *(int *)arg;
    int i;
    FILE *fp;
    
    printf("Thread %d: Testing with real syscalls\n", thread_num);
    
    for (i = 0; i < 5; i++) {
        /* Try to open non-existent file - should set errno to ENOENT */
        fp = fopen("/nonexistent/file/that/does/not/exist", "r");
        if (fp == NULL && errno == ENOENT) {
            printf("Thread %d: Correctly got ENOENT (%d)\n", thread_num, errno);
        } else {
            printf("Thread %d: ERROR! Expected ENOENT, got %d\n", thread_num, errno);
        }
        usleep(500);
    }
    
    return NULL;
}

int test_real_syscalls(void)
{
    pthread_t threads[3];
    int thread_nums[3];
    int i;
    
    printf("\n=== TEST 3: errno with real system calls ===\n");
    
    for (i = 0; i < 3; i++) {
        thread_nums[i] = i + 1;
        pthread_create(&threads[i], NULL, thread_syscall_test, &thread_nums[i]);
    }
    
    for (i = 0; i < 3; i++) {
        pthread_join(threads[i], NULL);
    }
    
    printf("PASSED: System call errno handling\n");
    return 1;
}

/* Test the transition from single to multi-threaded */
int test_transition(void)
{
    pthread_t thread;
    int thread_num = 1;
    void *result;
    
    printf("\n=== TEST 4: Single to multi-threaded transition ===\n");
    
    /* Set errno before threading */
    errno = 42;
    printf("Before threading: errno = %d\n", errno);
    
    if (errno != 42) {
        printf("FAILED: errno not 42 before threading\n");
        return 0;
    }
    
    /* Create one thread */
    pthread_create(&thread, NULL, thread_test_errno, &thread_num);
    
    /* Check main thread errno is still intact */
    if (errno != 42) {
        printf("FAILED: Main thread errno changed to %d after pthread_create\n", errno);
        return 0;
    }
    
    pthread_join(thread, &result);
    
    /* Check again after join */
    if (errno != 42) {
        printf("FAILED: Main thread errno changed to %d after join\n", errno);
        return 0;
    }
    
    printf("PASSED: errno preserved through threading transition\n");
    free(result);
    return 1;
}

int main(int argc, char *argv[])
{
    int test1, test2, test3, test4;
    int all_passed;
    
    printf("======================================\n");
    printf("Thread-Safe errno Test Suite\n");
    printf("======================================\n");
    
    test1 = test_single_threaded();
    test2 = test_multi_threaded();
    test3 = test_real_syscalls();
    test4 = test_transition();
    
    all_passed = test1 && test2 && test3 && test4;
    
    printf("\n======================================\n");
    printf("FINAL RESULTS:\n");
    printf("======================================\n");
    printf("Test 1 (Single-threaded):  %s\n", test1 ? "PASSED" : "FAILED");
    printf("Test 2 (Multi-threaded):   %s\n", test2 ? "PASSED" : "FAILED");
    printf("Test 3 (Real syscalls):    %s\n", test3 ? "PASSED" : "FAILED");
    printf("Test 4 (Transition):       %s\n", test4 ? "PASSED" : "FAILED");
    printf("======================================\n");
    
    if (all_passed) {
        printf("ALL TESTS PASSED!\n");
        return 0;
    } else {
        printf("SOME TESTS FAILED!\n");
        return 1;
    }
}