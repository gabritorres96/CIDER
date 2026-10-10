/* Four threads increment a shared counter 10000 times each. The mutex is statically initialised (PTHREAD_MUTEX_INITIALIZER, a
   Darwin signature that CIDER recognises lazily). sched_yield() inside the critical section forces other threads to be scheduled
   while the lock is held, so only real mutual exclusion keeps the count exact. */
#include "darwin.h"

#define THREADS 4
#define ROUNDS 10000
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static long counter;
static int max_inside, inside;

static void *worker(void *arg) {
    for (int i = 0; i < ROUNDS; ++i) {
        pthread_mutex_lock(&lock);
        ++inside;
        if (inside > max_inside) max_inside = inside;
        long v = counter;
        if (i % 500 == 0) sched_yield();         /* other threads run now; they must block on the mutex */
        counter = v + 1;
        --inside;
        pthread_mutex_unlock(&lock);
    }
    return 0;
}

int main(void) {
    pthread_t t[THREADS];
    for (int i = 0; i < THREADS; ++i)
        if (pthread_create(&t[i], 0, worker, 0) != 0) return 1;
    for (int i = 0; i < THREADS; ++i)
        if (pthread_join(t[i], 0) != 0) return 2;
    printf("counter=%ld max_inside=%d\n", counter, max_inside);
    if (max_inside != 1) return 3;
    return counter == THREADS * ROUNDS ? 42 : 4;
}
