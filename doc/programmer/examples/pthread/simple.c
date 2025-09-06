#include <stdio.h>
//#include <mint/mintbind.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>

#include <pthread.h>

static inline int is_supervisor_mode(void) {
    unsigned short sr;
    __asm__ volatile ("move.w %%sr,%0" : "=d" (sr));
    return (sr & 0x2000) != 0;
}

// Global flag for synchronization
volatile int thread_done = 0;
volatile int counter = 0;

// Thread function that increments a counter
void counter_thread(void *arg) {
    int thread_num = (int)(long)arg;
    
    for (int i = 0; i < 10; i++) {
        counter++;
        printf("Thread %d: counter = %d, mode = %s\n", 
               thread_num, counter, 
               is_supervisor_mode() ? "SUPERVISOR" : "USER");
    }

    printf("thread %d out of for loop\n", thread_num);
    
    if (thread_num == 1) {
        printf("thread 1 executed\n");
        thread_done = 1;
    }

    pthread_exit(NULL);
}

int main(void) {

    pthread_t thread1, thread2;
    int result;

    printf("Main: TEST THREADS\n");
    
    result = pthread_create(&thread1, NULL, (void *(*)(void *))counter_thread, (void*)1);
    if (result != 0) {
        printf("Error creating 1st thread: %d\n", result);
        return 1;
    }
    
    result = pthread_create(&thread2, NULL, (void *(*)(void *))counter_thread, (void*)2);
    if (result != 0) {
        printf("Error creating 2nd thread: %d\n", result);
        return 1;
    }

    printf("Created threads: %ld, %ld\n", (long)thread1, (long)thread2);

    int timeout = 0;

    pthread_join(thread1, NULL);
    pthread_join(thread2, NULL);

    printf("Main: threads %s, final counter = %d\n", 
           thread_done ? "completed" : "timed out", counter);
    printf("Test completed\n");
    
    return 0;
}
