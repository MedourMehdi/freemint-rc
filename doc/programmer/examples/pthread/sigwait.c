/*
 * sigwait_test.c - Comprehensive test suite for sigwait()
 * 
 * Compile:
 *   Without threads: gcc -o sigwait_test_st sigwait_test.c
 *   With threads:    gcc -o sigwait_test_mt sigwait_test.c -pthread
 */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>

// #ifdef _REENTRANT
#include <pthread.h>
#define THREADED 1
// #else
// #define THREADED 0
// #endif

/* Test results */
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_START(name) \
    do { \
        printf("\n=== TEST: %s ===\n", name); \
    } while(0)

#define TEST_ASSERT(cond, msg) \
    do { \
        if (cond) { \
            printf("  [PASS] %s\n", msg); \
            tests_passed++; \
        } else { \
            printf("  [FAIL] %s\n", msg); \
            tests_failed++; \
        } \
    } while(0)

#define TEST_SUMMARY() \
    do { \
        printf("\n==============================\n"); \
        printf("Tests passed: %d\n", tests_passed); \
        printf("Tests failed: %d\n", tests_failed); \
        printf("==============================\n"); \
    } while(0)

/* Global flag for signal delivery tracking */
static volatile sig_atomic_t signal_received = 0;
static volatile sig_atomic_t signal_number = 0;

/* Simple signal handler for comparison tests */
void simple_handler(int sig)
{
    signal_received = 1;
    signal_number = sig;
}

/*
 * TEST 1: Basic sigwait functionality (single-threaded)
 */
void test_basic_sigwait(void)
{
    TEST_START("Basic sigwait - wait for SIGUSR1");
    
    sigset_t set;
    int sig;
    int ret;
    pid_t pid = getpid();
    
    /* Block SIGUSR1 */
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    ret = sigprocmask(SIG_BLOCK, &set, NULL);
    TEST_ASSERT(ret == 0, "Blocked SIGUSR1");
    
    /* Send signal to self */
    printf("  Sending SIGUSR1 to self (pid=%d)\n", pid);
    ret = kill(pid, SIGUSR1);
    TEST_ASSERT(ret == 0, "Signal sent successfully");
    
    /* Wait for signal */
    printf("  Calling sigwait()...\n");
    ret = sigwait(&set, &sig);
    TEST_ASSERT(ret == 0, "sigwait() returned 0");
    TEST_ASSERT(sig == SIGUSR1, "Received SIGUSR1");
    
    printf("  Received signal: %d\n", sig);
}

/*
 * TEST 2: Multiple signals with sigwait
 */
void test_multiple_signals(void)
{
    TEST_START("Multiple signals with sigwait");
    
    sigset_t set;
    int sig;
    int ret;
    pid_t pid = getpid();
    
    /* Block SIGUSR1 and SIGUSR2 */
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigaddset(&set, SIGUSR2);
    ret = sigprocmask(SIG_BLOCK, &set, NULL);
    TEST_ASSERT(ret == 0, "Blocked SIGUSR1 and SIGUSR2");
    
    /* Send SIGUSR2 first, then SIGUSR1 */
    printf("  Sending SIGUSR2\n");
    kill(pid, SIGUSR2);
    usleep(10000); /* 10ms delay */
    printf("  Sending SIGUSR1\n");
    kill(pid, SIGUSR1);
    
    /* Should receive SIGUSR2 first (sent first) */
    ret = sigwait(&set, &sig);
    TEST_ASSERT(ret == 0, "First sigwait() returned 0");
    printf("  First signal received: %d (expected SIGUSR2=%d)\n", sig, SIGUSR2);
    
    /* Should receive SIGUSR1 second */
    ret = sigwait(&set, &sig);
    TEST_ASSERT(ret == 0, "Second sigwait() returned 0");
    printf("  Second signal received: %d (expected SIGUSR1=%d)\n", sig, SIGUSR1);
}

/*
 * TEST 3: sigwait vs signal handler
 * When signal is blocked and sigwait is used, handler should NOT run
 */
void test_sigwait_vs_handler(void)
{
    TEST_START("sigwait prevents handler execution");
    
    sigset_t set;
    int sig;
    int ret;
    pid_t pid = getpid();
    struct sigaction sa;
    
    /* Reset global flags */
    signal_received = 0;
    signal_number = 0;
    
    /* Install handler for SIGUSR1 */
    sa.sa_handler = simple_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, NULL);
    
    /* Block SIGUSR1 */
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    ret = sigprocmask(SIG_BLOCK, &set, NULL);
    TEST_ASSERT(ret == 0, "Blocked SIGUSR1");
    
    /* Send signal */
    printf("  Sending SIGUSR1\n");
    kill(pid, SIGUSR1);
    usleep(10000); /* Give time for potential handler execution */
    
    TEST_ASSERT(signal_received == 0, "Handler was NOT called (signal blocked)");
    
    /* Use sigwait to consume the signal */
    ret = sigwait(&set, &sig);
    TEST_ASSERT(ret == 0, "sigwait() returned 0");
    TEST_ASSERT(sig == SIGUSR1, "Received SIGUSR1");
    TEST_ASSERT(signal_received == 0, "Handler still NOT called (consumed by sigwait)");
    
    /* Restore default handler */
    sa.sa_handler = SIG_DFL;
    sigaction(SIGUSR1, &sa, NULL);
}

/*
 * TEST 4: Invalid arguments
 */
void test_invalid_args(void)
{
    TEST_START("Invalid arguments to sigwait");
    
    sigset_t set;
    int sig;
    int ret;
    
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    
    /* NULL set */
    ret = sigwait(NULL, &sig);
    TEST_ASSERT(ret == EINVAL || ret == -1, "NULL set returns error");
    
    /* NULL sig pointer */
    ret = sigwait(&set, NULL);
    TEST_ASSERT(ret == EINVAL || ret == -1, "NULL sig pointer returns error");
}

#if THREADED
/*
 * THREADED TEST 1: Basic thread signal delivery
 */
typedef struct {
    int thread_id;
    int signal_to_wait;
    int signal_received;
    int success;
} thread_test_data_t;

void* thread_sigwait_worker(void* arg)
{
    thread_test_data_t* data = (thread_test_data_t*)arg;
    sigset_t set;
    int sig;
    int ret;
    
    printf("  Thread %d: Started, waiting for signal %d\n", 
           data->thread_id, data->signal_to_wait);
    
    /* Block the signal we're waiting for */
    sigemptyset(&set);
    sigaddset(&set, data->signal_to_wait);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
    
    /* Wait for signal */
    ret = sigwait(&set, &sig);
    
    if (ret == 0 && sig == data->signal_to_wait) {
        data->signal_received = sig;
        data->success = 1;
        printf("  Thread %d: Received signal %d\n", data->thread_id, sig);
    } else {
        data->success = 0;
        printf("  Thread %d: FAILED - ret=%d, sig=%d\n", data->thread_id, ret, sig);
    }
    
    return NULL;
}

void test_threaded_sigwait(void)
{
    TEST_START("Threaded sigwait - multiple threads");
    
    pthread_t thread1, thread2;
    thread_test_data_t data1 = {1, SIGUSR1, 0, 0};
    thread_test_data_t data2 = {2, SIGUSR2, 0, 0};
    
    /* Create threads */
    pthread_create(&thread1, NULL, thread_sigwait_worker, &data1);
    pthread_create(&thread2, NULL, thread_sigwait_worker, &data2);
    
    /* Give threads time to start and block */
    sleep(1);
    
    /* Send signals */
    printf("  Main: Sending SIGUSR1\n");
    pthread_kill(thread1, SIGUSR1);
    usleep(100000); /* 100ms */
    
    printf("  Main: Sending SIGUSR2\n");
    pthread_kill(thread2, SIGUSR2);
    
    /* Wait for threads */
    pthread_join(thread1, NULL);
    pthread_join(thread2, NULL);
    
    TEST_ASSERT(data1.success == 1, "Thread 1 received SIGUSR1");
    TEST_ASSERT(data2.success == 1, "Thread 2 received SIGUSR2");
    TEST_ASSERT(data1.signal_received == SIGUSR1, "Thread 1 got correct signal");
    TEST_ASSERT(data2.signal_received == SIGUSR2, "Thread 2 got correct signal");
}

/*
 * THREADED TEST 2: Signal broadcast - one signal, multiple waiters
 */
void* thread_broadcast_worker(void* arg)
{
    thread_test_data_t* data = (thread_test_data_t*)arg;
    sigset_t set;
    int sig;
    int ret;
    
    printf("  Thread %d: Waiting for SIGUSR1\n", data->thread_id);
    
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
    
    ret = sigwait(&set, &sig);
    
    if (ret == 0) {
        data->signal_received = sig;
        data->success = 1;
        printf("  Thread %d: Received signal %d\n", data->thread_id, sig);
    } else {
        data->success = 0;
        printf("  Thread %d: FAILED\n", data->thread_id);
    }
    
    return NULL;
}

void test_threaded_broadcast(void)
{
    TEST_START("Threaded broadcast - multiple waiters for same signal");
    
    pthread_t threads[3];
    thread_test_data_t data[3];
    int i;
    
    /* Create multiple threads waiting for SIGUSR1 */
    for (i = 0; i < 3; i++) {
        data[i].thread_id = i + 1;
        data[i].signal_to_wait = SIGUSR1;
        data[i].signal_received = 0;
        data[i].success = 0;
        pthread_create(&threads[i], NULL, thread_broadcast_worker, &data[i]);
    }
    
    sleep(1); /* Let threads start */
    
    /* Send signal to each thread */
    printf("  Main: Broadcasting SIGUSR1 to all threads\n");
    for (i = 0; i < 3; i++) {
        pthread_kill(threads[i], SIGUSR1);
        usleep(50000); /* 50ms between signals */
    }
    
    /* Wait for all threads */
    for (i = 0; i < 3; i++) {
        pthread_join(threads[i], NULL);
    }
    
    /* All threads should have received the signal */
    for (i = 0; i < 3; i++) {
        char msg[100];
        snprintf(msg, sizeof(msg), "Thread %d received signal", i + 1);
        TEST_ASSERT(data[i].success == 1, msg);
    }
}

/*
 * THREADED TEST 3: Thread isolation - signal to one thread only
 */
typedef struct {
    pthread_t thread_id;
    int should_receive;
    int did_receive;
} isolation_data_t;

void* thread_isolation_worker(void* arg)
{
    isolation_data_t* data = (isolation_data_t*)arg;
    sigset_t set;
    int sig;
    int ret;
    struct timespec timeout = {2, 0}; /* 2 second timeout */
    
    data->thread_id = pthread_self();
    
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
    
    printf("  Thread %p: Waiting (should_receive=%d)\n", 
           (void*)data->thread_id, data->should_receive);
    
    /* Use sigtimedwait so we don't block forever */
    ret = sigtimedwait(&set, NULL, &timeout);
    
    if (ret > 0) {
        data->did_receive = 1;
        printf("  Thread %p: Received signal\n", (void*)data->thread_id);
    } else {
        data->did_receive = 0;
        printf("  Thread %p: Timed out (no signal)\n", (void*)data->thread_id);
    }
    
    return NULL;
}

void test_threaded_isolation(void)
{
    TEST_START("Threaded isolation - signal to specific thread only");
    
    pthread_t thread1, thread2;
    isolation_data_t data1 = {0, 1, 0}; /* Should receive */
    isolation_data_t data2 = {0, 0, 0}; /* Should NOT receive */
    
    pthread_create(&thread1, NULL, thread_isolation_worker, &data1);
    pthread_create(&thread2, NULL, thread_isolation_worker, &data2);
    
    sleep(1); /* Let threads start */
    
    /* Send signal ONLY to thread1 */
    printf("  Main: Sending SIGUSR1 to thread1 only\n");
    pthread_kill(thread1, SIGUSR1);
    
    /* Wait for threads */
    pthread_join(thread1, NULL);
    pthread_join(thread2, NULL);
    
    TEST_ASSERT(data1.did_receive == 1, "Target thread received signal");
    TEST_ASSERT(data2.did_receive == 0, "Non-target thread did NOT receive signal");
}
#endif /* THREADED */

/*
 * Main test runner
 */
int main(int argc, char* argv[])
{
    printf("==============================\n");
    printf("sigwait() Test Suite\n");
#if THREADED
    printf("Mode: MULTI-THREADED\n");
#else
    printf("Mode: SINGLE-THREADED\n");
#endif
    printf("==============================\n");
    
    /* Single-threaded tests (run in both modes) */
    test_basic_sigwait();
    test_multiple_signals();
    test_sigwait_vs_handler();
    test_invalid_args();
    
#if THREADED
    /* Multi-threaded tests */
    test_threaded_sigwait();
    test_threaded_broadcast();
    test_threaded_isolation();
#endif
    
    /* Print summary */
    TEST_SUMMARY();
    
    return (tests_failed > 0) ? 1 : 0;
}
