/* Condition variables, error-checking and recursive mutexes, pthread_once, thread-local errno. */
#include "darwin.h"

static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int ready, consumed, produced;

static void *consumer(void *arg) {
    pthread_mutex_lock(&m);
    while (!ready) pthread_cond_wait(&cv, &m);       /* releases the mutex while waiting */
    consumed = produced * 2;
    pthread_mutex_unlock(&m);
    return 0;
}

static pthread_once_t once = PTHREAD_ONCE_INIT;
static int once_count;
static void init_once(void) { ++once_count; }
static void *once_worker(void *arg) { pthread_once(&once, init_once); return 0; }

static void *errno_worker(void *arg) {
    errno = 99;
    open("no-such-file-for-the-thread", O_RDONLY);    /* ENOENT in THIS thread only */
    return (void *)(long)errno;
}

int main(void) {
    pthread_t c;
    pthread_create(&c, 0, consumer, 0);
    sched_yield();                                    /* the consumer runs and waits on the condition */
    pthread_mutex_lock(&m);
    produced = 21;
    ready = 1;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&m);
    pthread_join(c, 0);
    if (consumed != 42) return 1;

    /* mutex kinds */
    pthread_mutex_t plain = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&plain);
    if (pthread_mutex_trylock(&plain) != EBUSY) return 2;
    if (pthread_mutex_lock(&plain) != EDEADLK) return 3;          /* relocking a normal mutex would deadlock: refused */
    pthread_mutex_unlock(&plain);
    if (pthread_mutex_unlock(&plain) != EPERM) return 4;          /* not locked */

    pthread_mutexattr_t attr;
    pthread_mutex_t rec;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&rec, &attr);
    if (pthread_mutex_lock(&rec) || pthread_mutex_lock(&rec) || pthread_mutex_lock(&rec)) return 5;
    pthread_mutex_unlock(&rec);
    pthread_mutex_unlock(&rec);
    if (pthread_mutex_destroy(&rec) != EBUSY) return 6;           /* still locked once */
    pthread_mutex_unlock(&rec);
    if (pthread_mutex_destroy(&rec) != 0) return 7;
    if (pthread_mutex_lock(&rec) == 0) return 8;                  /* destroyed: invalid */

    /* pthread_once from several threads */
    pthread_t t[3];
    for (int i = 0; i < 3; ++i) pthread_create(&t[i], 0, once_worker, 0);
    for (int i = 0; i < 3; ++i) pthread_join(t[i], 0);
    pthread_once(&once, init_once);
    if (once_count != 1) return 9;

    /* errno is per thread */
    errno = 7;
    void *r = 0;
    pthread_t e;
    pthread_create(&e, 0, errno_worker, 0);
    pthread_join(e, &r);
    if ((long)r != ENOENT || errno != 7) return 10;
    return 42;
}
