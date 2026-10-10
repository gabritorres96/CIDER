/* The same counter WITHOUT the mutex, yielding between the load and the store: updates are lost. This proves that the guest threads
   really interleave (at the yield points) instead of running one after the other, and shows what the mutex test protects against. */
#include "darwin.h"

#define THREADS 4
#define ROUNDS 1000
static volatile long counter;

static void *worker(void *arg) {
    for (int i = 0; i < ROUNDS; ++i) {
        long v = counter;
        sched_yield();
        counter = v + 1;
    }
    return 0;
}

int main(void) {
    pthread_t t[THREADS];
    for (int i = 0; i < THREADS; ++i) pthread_create(&t[i], 0, worker, 0);
    for (int i = 0; i < THREADS; ++i) pthread_join(t[i], 0);
    printf("counter=%ld of %d\n", counter, THREADS * ROUNDS);
    return counter < THREADS * ROUNDS ? 42 : 1;
}
