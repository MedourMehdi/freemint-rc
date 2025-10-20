/*
 * test_thread_signals.c
 * 
 * Test program for thread-aware signal handling in FreeMiNT
 * 
 * Compile with:
 *   m68k-atari-mint-gcc -o test_signals test_thread_signals.c -lpthread
 */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sys/types.h>

/* Signal handler counters */
volatile int main_sigusr1_count = 0;
volatile int main_sigusr2_count = 0;
volatile int thread1_sigusr1_count = 0;
volatile int thread2_sigusr1_count = 0;

/* Thread info structure */
typedef struct {
    int thread_num;
    pthread_t tid;
} thread_info_t;

/* Signal handler for main thread */
void main_sigusr1_handler(int sig)
{
    main_sigusr1_count++;
    printf("MAIN: Received SIGUSR1 (count=%d)\n", main_sigusr1_count);
}

void main_sigusr2_handler(int sig)
{
    main_sigusr2_count++;
    printf("MAIN: Received SIGUSR2 (count=%d)\n", main_sigusr2_count);
}

/* Signal handler for thread 1 */
void thread1_sigusr1_handler(int sig)
{
    thread1_sigusr1_count++;
    printf("THREAD1: Received SIGUSR1 (count=%d)\n", thread1_sigusr1_count);
}

/* Signal handler for thread 2 */
void thread2_sigusr1_handler(int sig)
{
    thread2_sigusr1_count++;
    printf("THREAD2: Received SIGUSR1 (count=%d)\n", thread2_sigusr1_count);
}

/* Thread 1: Sets its own SIGUSR1 handler */
void *thread1_func(void *arg)
{
    thread_info_t *info = (thread_info_t *)arg;
    void (*old_handler)(int);
    
    printf("Thread %d: Starting (pthread_id=%lu)\n", info->thread_num, 
           (unsigned long)info->tid);
    
    /* Set thread-specific handler for SIGUSR1 */
    old_handler = signal(SIGUSR1, thread1_sigusr1_handler);
    printf("Thread %d: Set SIGUSR1 handler (old=%p, new=%p)\n", 
           info->thread_num, old_handler, thread1_sigusr1_handler);
    
    /* Wait for signals */
    printf("Thread %d: Waiting for signals...\n", info->thread_num);
    sleep(5);
    
    printf("Thread %d: Finished (received %d SIGUSR1 signals)\n", 
           info->thread_num, thread1_sigusr1_count);
    return NULL;
}

/* Thread 2: Sets its own SIGUSR1 handler */
void *thread2_func(void *arg)
{
    thread_info_t *info = (thread_info_t *)arg;
    void (*old_handler)(int);
    
    printf("Thread %d: Starting (pthread_id=%lu)\n", info->thread_num,
           (unsigned long)info->tid);
    
    /* Set thread-specific handler for SIGUSR1 */
    old_handler = signal(SIGUSR1, thread2_sigusr1_handler);
    printf("Thread %d: Set SIGUSR1 handler (old=%p, new=%p)\n",
           info->thread_num, old_handler, thread2_sigusr1_handler);
    
    /* Wait for signals */
    printf("Thread %d: Waiting for signals...\n", info->thread_num);
    sleep(5);
    
    printf("Thread %d: Finished (received %d SIGUSR1 signals)\n",
           info->thread_num, thread2_sigusr1_count);
    return NULL;
}

/* Thread 3: Ignores SIGUSR1 */
void *thread3_func(void *arg)
{
    thread_info_t *info = (thread_info_t *)arg;
    void (*old_handler)(int);
    
    printf("Thread %d: Starting (pthread_id=%lu)\n", info->thread_num,
           (unsigned long)info->tid);
    
    /* Ignore SIGUSR1 for this thread */
    old_handler = signal(SIGUSR1, SIG_IGN);
    printf("Thread %d: Set SIGUSR1 to SIG_IGN (old=%p)\n",
           info->thread_num, old_handler);
    
    /* Wait for signals (should not receive any) */
    printf("Thread %d: Waiting (should ignore signals)...\n", info->thread_num);
    sleep(5);
    
    printf("Thread %d: Finished (should have ignored all signals)\n",
           info->thread_num);
    return NULL;
}

/* Thread 4: Resets handler to default */
void *thread4_func(void *arg)
{
    thread_info_t *info = (thread_info_t *)arg;
    void (*old_handler)(int);
    
    printf("Thread %d: Starting (pthread_id=%lu)\n", info->thread_num,
           (unsigned long)info->tid);
    
    /* First set a handler */
    signal(SIGUSR1, thread1_sigusr1_handler);
    printf("Thread %d: Set initial handler\n", info->thread_num);
    
    sleep(1);
    
    /* Reset to default */
    old_handler = signal(SIGUSR1, SIG_DFL);
    printf("Thread %d: Reset to SIG_DFL (old=%p)\n",
           info->thread_num, old_handler);
    
    /* Wait - should use process-level handler now */
    printf("Thread %d: Waiting (should use process handler)...\n", info->thread_num);
    sleep(4);
    
    printf("Thread %d: Finished\n", info->thread_num);
    return NULL;
}

int main(void)
{
    pthread_t threads[4];
    thread_info_t thread_infos[4];
    int i, ret;
    pid_t pid = getpid();
    
    printf("=== Thread-Aware Signal Handler Test ===\n");
    printf("Process PID: %d\n\n", pid);
    
    /* Set main thread handlers */
    printf("MAIN: Setting signal handlers\n");
    signal(SIGUSR1, main_sigusr1_handler);
    signal(SIGUSR2, main_sigusr2_handler);
    
    /* Create threads */
    printf("\n--- Creating threads ---\n");
    
    /* Thread 1: Custom SIGUSR1 handler */
    thread_infos[0].thread_num = 1;
    ret = pthread_create(&threads[0], NULL, thread1_func, &thread_infos[0]);
    if (ret != 0) {
        fprintf(stderr, "Failed to create thread 1: %s\n", strerror(ret));
        return 1;
    }
    thread_infos[0].tid = threads[0];
    
    /* Thread 2: Different custom SIGUSR1 handler */
    thread_infos[1].thread_num = 2;
    ret = pthread_create(&threads[1], NULL, thread2_func, &thread_infos[1]);
    if (ret != 0) {
        fprintf(stderr, "Failed to create thread 2: %s\n", strerror(ret));
        return 1;
    }
    thread_infos[1].tid = threads[1];
    
    /* Thread 3: Ignores SIGUSR1 */
    thread_infos[2].thread_num = 3;
    ret = pthread_create(&threads[2], NULL, thread3_func, &thread_infos[2]);
    if (ret != 0) {
        fprintf(stderr, "Failed to create thread 3: %s\n", strerror(ret));
        return 1;
    }
    thread_infos[2].tid = threads[2];
    
    /* Thread 4: Resets to default */
    thread_infos[3].thread_num = 4;
    ret = pthread_create(&threads[3], NULL, thread4_func, &thread_infos[3]);
    if (ret != 0) {
        fprintf(stderr, "Failed to create thread 4: %s\n", strerror(ret));
        return 1;
    }
    thread_infos[3].tid = threads[3];
    
    /* Give threads time to set up handlers */
    sleep(2);
    
    /* Send signals */
    printf("\n--- Sending signals ---\n");
    printf("Send SIGUSR1 to process (should go to main or threads with handlers)\n");
    kill(pid, SIGUSR1);
    usleep(100000); /* 100ms */
    
    printf("Send SIGUSR1 again\n");
    kill(pid, SIGUSR1);
    usleep(100000);
    
    printf("Send SIGUSR2 to process (only main has handler)\n");
    kill(pid, SIGUSR2);
    usleep(100000);
    
    printf("Send SIGUSR1 one more time\n");
    kill(pid, SIGUSR1);
    usleep(100000);
    
    /* Wait for threads to finish */
    printf("\n--- Waiting for threads to finish ---\n");
    for (i = 0; i < 4; i++) {
        ret = pthread_join(threads[i], NULL);
        if (ret != 0) {
            fprintf(stderr, "Failed to join thread %d: %s\n", i+1, strerror(ret));
        }
    }
    
    /* Print results */
    printf("\n=== Results ===\n");
    printf("Main thread SIGUSR1 count: %d\n", main_sigusr1_count);
    printf("Main thread SIGUSR2 count: %d\n", main_sigusr2_count);
    printf("Thread 1 SIGUSR1 count: %d\n", thread1_sigusr1_count);
    printf("Thread 2 SIGUSR1 count: %d\n", thread2_sigusr1_count);
    printf("Thread 3 should have ignored all signals\n");
    printf("Thread 4 should have used process handler after reset\n");
    
    printf("\n=== Expected behavior ===\n");
    printf("- Each thread with a custom handler should receive some SIGUSR1 signals\n");
    printf("- Thread 3 (SIG_IGN) should not increment any counter\n");
    printf("- Thread 4 should fall back to main handler after SIG_DFL\n");
    printf("- SIGUSR2 should only be handled by main thread\n");
    printf("- Total SIGUSR1 signals sent: 3\n");
    
    /* Validate results */
    printf("\n=== Validation ===\n");
    int total_sigusr1 = main_sigusr1_count + thread1_sigusr1_count + thread2_sigusr1_count;
    if (total_sigusr1 > 0) {
        printf("PASS: At least one SIGUSR1 was received\n");
    } else {
        printf("FAIL: No SIGUSR1 signals received\n");
    }
    
    if (main_sigusr2_count > 0) {
        printf("PASS: SIGUSR2 received by main thread\n");
    } else {
        printf("FAIL: SIGUSR2 not received\n");
    }
    
    if (thread1_sigusr1_count > 0 || thread2_sigusr1_count > 0) {
        printf("PASS: Thread-specific handlers worked\n");
    } else {
        printf("WARNING: No thread-specific handlers were called\n");
    }
    
    printf("\n=== Test complete ===\n");
    return 0;
}