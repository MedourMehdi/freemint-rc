/**
 * @file test_thread_signals.c
 * @brief POSIX-compliant test suite for thread signal handling
 * 
 * Uses ONLY standard POSIX pthread and signal functions.
 * Tests behavior that should work with your kernel implementation.
 * 
 * Compile: gcc -o test_signals test_thread_signals.c -lpthread
 * Run: ./test_signals
 */

// #define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <assert.h>

/* Test result tracking */
typedef struct {
    int passed;
    int failed;
    int skipped;
} test_results_t;

static test_results_t results = {0, 0, 0};

/* Test utilities */
#define TEST_START(name) \
    printf("\n=== TEST: %s ===\n", name); \
    fflush(stdout);

#define TEST_PASS(name) \
    do { \
        printf("✓ PASS: %s\n", name); \
        results.passed++; \
        fflush(stdout); \
    } while(0)

#define TEST_FAIL(name, reason) \
    do { \
        printf("✗ FAIL: %s - %s\n", name, reason); \
        results.failed++; \
        fflush(stdout); \
    } while(0)

#define TEST_SKIP(name, reason) \
    do { \
        printf("⊘ SKIP: %s - %s\n", name, reason); \
        results.skipped++; \
        fflush(stdout); \
    } while(0)

#define ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            TEST_FAIL(__func__, message); \
            return; \
        } \
    } while(0)

#define ASSERT_EQ(actual, expected, message) \
    do { \
        if ((actual) != (expected)) { \
            char buf[256]; \
            snprintf(buf, sizeof(buf), "%s (expected %ld, got %ld)", \
                    message, (long)(expected), (long)(actual)); \
            TEST_FAIL(__func__, buf); \
            return; \
        } \
    } while(0)

/* Test data structures */
typedef struct {
    volatile sig_atomic_t signal_received;
    volatile sig_atomic_t signal_number;
    volatile sig_atomic_t handler_count;
    pthread_t thread_id;
    volatile sig_atomic_t in_handler;
} signal_test_data_t;

static signal_test_data_t test_data;

/* POSIX signal handlers */
void simple_signal_handler(int sig) {
    printf("  [Handler] Signal %d received by thread 0x%lx\n", 
           sig, (unsigned long)pthread_self());
    test_data.signal_received = 1;
    test_data.signal_number = sig;
    test_data.handler_count++;
    printf("  [Handler] Signal %d received by thread 0x%lx\n", 
           sig, (unsigned long)pthread_self());
    fflush(stdout);
}

void counting_signal_handler(int sig) {
    test_data.handler_count++;
    printf("  [Handler] Signal %d, count: %d\n", sig, test_data.handler_count);
    fflush(stdout);
}

void blocking_signal_handler(int sig) {
    test_data.in_handler = 1;
    printf("  [Handler] Blocking handler started\n");
    fflush(stdout);
    usleep(100000);  /* 100ms */
    test_data.in_handler = 0;
    printf("  [Handler] Blocking handler finished\n");
    fflush(stdout);
}

/* ============================================================================
 * TEST 1: Basic Signal Handler Registration and Delivery
 * ============================================================================ */
void test_basic_signal_handler(void) {
    TEST_START("Basic Signal Handler Registration");
    
    memset(&test_data, 0, sizeof(test_data));
    
    /* Set up signal handler using POSIX sigaction */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = simple_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    
    int ret = sigaction(SIGUSR1, &sa, NULL);
    ASSERT_EQ(ret, 0, "sigaction() failed");
    
    /* Send signal to self */
    ret = pthread_kill(pthread_self(), SIGUSR1);
    ASSERT_EQ(ret, 0, "pthread_kill() to self failed");
    
    /* Wait for signal delivery */
    sleep(5);  /* 50ms */
    
    ASSERT(test_data.signal_received == 1, "Signal not received");
    ASSERT_EQ(test_data.signal_number, SIGUSR1, "Wrong signal number");
    
    TEST_PASS("Basic Signal Handler Registration");
}

/* ============================================================================
 * TEST 2: pthread_sigmask - Block and Unblock Signals
 * ============================================================================ */
void test_pthread_sigmask(void) {
    TEST_START("pthread_sigmask - Block/Unblock");
    
    memset(&test_data, 0, sizeof(test_data));
    
    /* Set up handler */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = simple_signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    
    /* Block SIGUSR1 */
    sigset_t mask, oldmask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    
    int ret = pthread_sigmask(SIG_BLOCK, &mask, &oldmask);
    ASSERT_EQ(ret, 0, "pthread_sigmask(SIG_BLOCK) failed");
    
    /* Send signal - should be blocked */
    ret = pthread_kill(pthread_self(), SIGUSR1);
    ASSERT_EQ(ret, 0, "pthread_kill() failed");
    
    usleep(50000);  /* 50ms */
    ASSERT(test_data.signal_received == 0, "Signal should be blocked");
    
    /* Unblock signal */
    ret = pthread_sigmask(SIG_UNBLOCK, &mask, NULL);
    ASSERT_EQ(ret, 0, "pthread_sigmask(SIG_UNBLOCK) failed");
    
    usleep(50000);  /* 50ms */
    ASSERT(test_data.signal_received == 1, "Signal should be delivered after unblock");
    
    TEST_PASS("pthread_sigmask - Block/Unblock");
}

/* ============================================================================
 * TEST 3: Thread-Specific Signal Delivery
 * ============================================================================ */
void *target_thread_func(void *arg) {
    signal_test_data_t *data = (signal_test_data_t *)arg;
    data->thread_id = pthread_self();
    
    /* Set up handler in this thread */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = simple_signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR2, &sa, NULL);
    
    printf("  [Thread] Waiting for signal...\n");
    fflush(stdout);
    
    /* Sleep to allow signal delivery */
    sleep(10);
    
    printf("  [Thread] Exiting (received: %d)\n", data->signal_received);
    fflush(stdout);
    
    return NULL;
}

void test_thread_specific_signal(void) {
    TEST_START("Thread-Specific Signal Delivery");
    
    memset(&test_data, 0, sizeof(test_data));
    
    pthread_t thread;
    int ret = pthread_create(&thread, NULL, target_thread_func, &test_data);
    ASSERT(ret == 0, "pthread_create() failed");
    
    /* Wait for thread to start */
    usleep(100000);  /* 100ms */
    
    /* Send signal to specific thread using pthread_kill */
    ret = pthread_kill(thread, SIGUSR2);
    ASSERT_EQ(ret, 0, "pthread_kill() to thread failed");
    
    /* Join thread */
    pthread_join(thread, NULL);
    
    ASSERT(test_data.signal_received == 1, "Signal not received by target thread");
    
    TEST_PASS("Thread-Specific Signal Delivery");
}

/* ============================================================================
 * TEST 4: sigwait() - Synchronous Signal Reception
 * ============================================================================ */
void *sigwait_thread_func(void *arg) {
    signal_test_data_t *data = (signal_test_data_t *)arg;
    
    /* Block SIGUSR1 - required for sigwait */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);
    
    printf("  [Thread] Waiting for signal with sigwait()...\n");
    fflush(stdout);
    
    int sig;
    printf("  [Thread] Calling sigwait(), mask = { SIGUSR1 }\n");
    int ret = sigwait(&mask, &sig);
    // int ret = pthread_sigwait(&mask, &sig);
    printf("  [Thread] sigwait() returned (ret=%d, sig=%d)\n", ret, sig);
    if (ret == 0) {
        data->signal_received = 1;
        data->signal_number = sig;
        printf("  [Thread] sigwait() returned signal %d\n", sig);
    } else {
        printf("  [Thread] sigwait() failed with error %d\n", ret);
    }
    fflush(stdout);
    
    return NULL;
}

void test_sigwait(void) {
    TEST_START("sigwait() - Synchronous Signal Reception");
    
    memset(&test_data, 0, sizeof(test_data));
    
    pthread_t thread;
    int ret = pthread_create(&thread, NULL, sigwait_thread_func, &test_data);
    ASSERT(ret == 0, "pthread_create() failed");
    
    /* Give thread time to enter sigwait */
    usleep(100000);  /* 100ms */
    // msleep(300);  /* 300ms */
    
    /* Send signal to thread */
    ret = pthread_kill(thread, SIGUSR1);
    ASSERT_EQ(ret, 0, "pthread_kill() failed");
    
    pthread_join(thread, NULL);
    
    ASSERT(test_data.signal_received == 1, "sigwait() did not receive signal");
    ASSERT_EQ(test_data.signal_number, SIGUSR1, "Wrong signal received");
    
    TEST_PASS("sigwait() - Synchronous Signal Reception");
}

/* ============================================================================
 * TEST 5: Signal Interrupting Sleep
 * ============================================================================ */
void *sleeping_thread_func(void *arg) {
    signal_test_data_t *data = (signal_test_data_t *)arg;
    
    /* Set up handler */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = simple_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  /* No SA_RESTART - let sleep be interrupted */
    sigaction(SIGUSR1, &sa, NULL);
    
    printf("  [Thread] Sleeping for 5 seconds...\n");
    fflush(stdout);
    
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    /* Sleep should be interrupted by signal */
    int remaining = sleep(5);
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    long elapsed_ms = (end.tv_sec - start.tv_sec) * 1000 + 
                      (end.tv_nsec - start.tv_nsec) / 1000000;
    
    printf("  [Thread] Woke up after %ld ms (remaining: %d sec)\n", 
           elapsed_ms, remaining);
    fflush(stdout);
    
    return (void*)(long)elapsed_ms;
}

void test_signal_interrupt_sleep(void) {
    TEST_START("Signal Interrupting Sleep");
    
    memset(&test_data, 0, sizeof(test_data));
    
    pthread_t thread;
    printf("  [Parent] Creating sleeping thread...\n");
    int ret = pthread_create(&thread, NULL, sleeping_thread_func, &test_data);
    ASSERT(ret == 0, "pthread_create() failed");
    
    /* Wait then send signal */
    printf("  [Parent] Sleeping 1 seconds before sending signal...\n");
    sleep(1); 
    
    printf("  [Parent] Sending SIGUSR1 to sleeping thread...\n");
    ret = pthread_kill(thread, SIGUSR1);
    ASSERT_EQ(ret, 0, "pthread_kill() failed");
    
    void *result;
    pthread_join(thread, &result);
    long elapsed_ms = (long)result;
    
    ASSERT(test_data.signal_received == 1, "Signal not received");
    ASSERT(elapsed_ms < 4000, "Sleep was not interrupted by signal");
    
    printf("  Sleep interrupted after %ld ms (expected < 4000 ms)\n", elapsed_ms);
    fflush(stdout);
    
    TEST_PASS("Signal Interrupting Sleep");
}

/* ============================================================================
 * TEST 6: Multiple Signals to Same Thread
 * ============================================================================ */
void test_multiple_signals(void) {
    TEST_START("Multiple Signals to Same Thread");
    
    memset(&test_data, 0, sizeof(test_data));
    
    /* Set up handler */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = counting_signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    
    /* Send multiple signals */
    const int num_signals = 5;
    for (int i = 0; i < num_signals; i++) {
        int ret = pthread_kill(pthread_self(), SIGUSR1);
        ASSERT_EQ(ret, 0, "pthread_kill() failed");
        usleep(20000);  /* 20ms between signals */
    }
    
    usleep(100000);  /* 100ms to ensure all processed */
    
    printf("  Sent %d signals, handler called %d times\n", 
           num_signals, test_data.handler_count);
    fflush(stdout);
    
    ASSERT(test_data.handler_count >= 1, "No signals delivered");
    
    /* Note: POSIX allows signal coalescing for standard signals */
    if (test_data.handler_count < num_signals) {
        printf("  (Signal coalescing occurred - this is POSIX-compliant)\n");
        fflush(stdout);
    }
    
    TEST_PASS("Multiple Signals to Same Thread");
}

/* ============================================================================
 * TEST 7: Signal Masking Across Thread Creation
 * ============================================================================ */
void *inheriting_thread_func(void *arg) {
    signal_test_data_t *data = (signal_test_data_t *)arg;
    
    /* Check inherited signal mask */
    sigset_t current_mask;
    pthread_sigmask(SIG_SETMASK, NULL, &current_mask);
    
    if (sigismember(&current_mask, SIGUSR1)) {
        printf("  [Thread] SIGUSR1 is blocked (inherited from parent)\n");
        data->signal_received = 1;  /* Use as flag for "mask inherited" */
    } else {
        printf("  [Thread] SIGUSR1 is NOT blocked\n");
    }
    fflush(stdout);
    
    return NULL;
}

void test_signal_mask_inheritance(void) {
    TEST_START("Signal Mask Inheritance Across Thread Creation");
    
    memset(&test_data, 0, sizeof(test_data));
    
    /* Block SIGUSR1 in parent */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);
    
    printf("  [Parent] Blocked SIGUSR1\n");
    fflush(stdout);
    
    /* Create child thread - should inherit mask */
    pthread_t thread;
    int ret = pthread_create(&thread, NULL, inheriting_thread_func, &test_data);
    ASSERT(ret == 0, "pthread_create() failed");
    
    pthread_join(thread, NULL);
    
    ASSERT(test_data.signal_received == 1, "Signal mask not inherited by child thread");
    
    /* Unblock for cleanup */
    pthread_sigmask(SIG_UNBLOCK, &mask, NULL);
    
    TEST_PASS("Signal Mask Inheritance Across Thread Creation");
}

/* ============================================================================
 * TEST 8: Signal to Non-Existent Thread
 * ============================================================================ */
void *quick_exit_thread_func(void *arg) {
    usleep(10000);  /* 10ms */
    return NULL;
}

void test_signal_to_dead_thread(void) {
    TEST_START("Signal to Non-Existent Thread");
    
    pthread_t thread;
    int ret = pthread_create(&thread, NULL, quick_exit_thread_func, NULL);
    ASSERT(ret == 0, "pthread_create() failed");
    
    /* Wait for thread to exit */
    pthread_join(thread, NULL);
    
    /* Try to send signal to dead thread */
    ret = pthread_kill(thread, SIGUSR1);
    
    printf("  pthread_kill() to dead thread returned: %d (errno: %s)\n", 
           ret, ret != 0 ? strerror(ret) : "success");
    fflush(stdout);
    
    /* POSIX says this should return ESRCH (no such process) */
    if (ret == ESRCH) {
        TEST_PASS("Signal to Non-Existent Thread");
    } else if (ret == 0) {
        printf("  Warning: pthread_kill() to dead thread succeeded (implementation-specific)\n");
        fflush(stdout);
        TEST_PASS("Signal to Non-Existent Thread");
    } else {
        TEST_FAIL("Signal to Non-Existent Thread", "Unexpected error code");
    }
}

/* ============================================================================
 * TEST 9: Concurrent Signals from Multiple Threads
 * ============================================================================ */
void *signal_sender_thread_func(void *arg) {
    pthread_t target = *(pthread_t *)arg;
    
    for (int i = 0; i < 10; i++) {
        pthread_kill(target, SIGUSR1);
        usleep(5000);  /* 5ms */
    }
    
    return NULL;
}

void test_concurrent_signal_delivery(void) {
    TEST_START("Concurrent Signal Delivery from Multiple Threads");
    
    memset(&test_data, 0, sizeof(test_data));
    
    /* Set up handler */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = counting_signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    
    pthread_t target = pthread_self();
    const int num_senders = 3;
    pthread_t senders[num_senders];
    
    /* Create multiple sender threads */
    for (int i = 0; i < num_senders; i++) {
        int ret = pthread_create(&senders[i], NULL, 
                                signal_sender_thread_func, &target);
        ASSERT(ret == 0, "pthread_create() failed");
    }
    
    /* Wait for all senders to complete */
    for (int i = 0; i < num_senders; i++) {
        pthread_join(senders[i], NULL);
    }
    
    usleep(200000);  /* 200ms to process all signals */
    
    printf("  Handler called %d times (from %d senders × 10 signals)\n", 
           test_data.handler_count, num_senders);
    fflush(stdout);
    
    ASSERT(test_data.handler_count > 0, "No signals delivered");
    
    TEST_PASS("Concurrent Signal Delivery from Multiple Threads");
}

/* ============================================================================
 * TEST 10: sigpending() - Check Pending Signals
 * ============================================================================ */
void test_sigpending(void) {
    TEST_START("sigpending() - Check Pending Signals");
    
    memset(&test_data, 0, sizeof(test_data));
    
    /* Block SIGUSR1 */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);
    
    /* Send signal - should be pending */
    int ret = pthread_kill(pthread_self(), SIGUSR1);
    ASSERT_EQ(ret, 0, "pthread_kill() failed");
    
    /* Check pending signals */
    sigset_t pending;
    ret = sigpending(&pending);
    ASSERT_EQ(ret, 0, "sigpending() failed");
    
    ASSERT(sigismember(&pending, SIGUSR1), "SIGUSR1 not marked as pending");
    
    printf("  SIGUSR1 is correctly marked as pending\n");
    fflush(stdout);
    
    /* Unblock to clear */
    pthread_sigmask(SIG_UNBLOCK, &mask, NULL);
    usleep(50000);
    
    TEST_PASS("sigpending() - Check Pending Signals");
}

/* ============================================================================
 * MAIN TEST RUNNER
 * ============================================================================ */

void print_test_summary(void) {
    printf("\n");
    printf("================================================================================\n");
    printf("                           TEST SUMMARY                                         \n");
    printf("================================================================================\n");
    printf("  PASSED:  %d\n", results.passed);
    printf("  FAILED:  %d\n", results.failed);
    printf("  SKIPPED: %d\n", results.skipped);
    printf("  TOTAL:   %d\n", results.passed + results.failed + results.skipped);
    printf("================================================================================\n");
    
    if (results.failed == 0) {
        printf("✓ ALL TESTS PASSED!\n");
    } else {
        printf("✗ SOME TESTS FAILED\n");
    }
    printf("\n");
}

int main(int argc, char **argv) {
    printf("================================================================================\n");
    printf("          POSIX Thread Signal Handling Test Suite                              \n");
    printf("================================================================================\n");
    printf("Testing standard POSIX pthread signal functions:\n");
    printf("  - pthread_kill()\n");
    printf("  - pthread_sigmask()\n");
    printf("  - sigwait()\n");
    printf("  - sigaction()\n");
    printf("  - sigpending()\n");
    printf("\n");
    
    /* Run all tests */
    test_basic_signal_handler();
    test_pthread_sigmask();
    test_thread_specific_signal();
    test_sigwait();
    test_signal_interrupt_sleep();
    test_multiple_signals();
    test_signal_mask_inheritance();
    test_signal_to_dead_thread();
    test_concurrent_signal_delivery();
    test_sigpending();
    
    // /* Print summary */
    print_test_summary();
    
    return (results.failed > 0) ? 1 : 0;
}
