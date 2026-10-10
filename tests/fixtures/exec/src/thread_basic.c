/* pthread_create / pthread_join / pthread_self / pthread_equal / pthread_exit / return values / attributes. */
#include "darwin.h"

static pthread_t seen_self;
static void *times_two(void *arg) {
    seen_self = pthread_self();
    return (void *)((long)arg * 2);
}
static void *exiter(void *arg) {
    pthread_exit((void *)((long)arg + 100));       /* does not return */
    return (void *)1;
}
static int ran_after_exit;
static void *deep_exit_helper(void *arg) { pthread_exit(arg); return 0; }
static void *deep_exit(void *arg) {
    deep_exit_helper(arg);                          /* pthread_exit from a nested guest frame unwinds the whole thread */
    ran_after_exit = 1;
    return 0;
}

int main(void) {
    pthread_t t;
    void *ret = 0;
    if (pthread_create(&t, 0, times_two, (void *)21) != 0) return 1;
    if (pthread_join(t, &ret) != 0) return 2;
    if ((long)ret != 42) return 3;
    if (seen_self == pthread_self() || seen_self == 0) return 4;      /* the thread had a different identity */
    if (pthread_equal(pthread_self(), pthread_self()) == 0) return 5;
    if (pthread_equal(t, pthread_self()) != 0) return 6;

    if (pthread_create(&t, 0, exiter, (void *)5) != 0) return 7;
    if (pthread_join(t, &ret) != 0 || (long)ret != 105) return 8;      /* pthread_exit value reaches join */

    if (pthread_create(&t, 0, deep_exit, (void *)77) != 0) return 9;
    if (pthread_join(t, &ret) != 0 || (long)ret != 77 || ran_after_exit) return 10;

    /* errors */
    if (pthread_join(t, &ret) != ESRCH) return 11;                       /* already joined */
    if (pthread_join(pthread_self(), &ret) != EDEADLK) return 12;        /* join self */
    if (pthread_create(&t, 0, (void *(*)(void *))0, 0) == 0) return 13;  /* NULL start routine */
    if (pthread_create(&t, 0, (void *(*)(void *))0x1000, 0) == 0) return 14;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    if (pthread_create(&t, &attr, times_two, (void *)4) != 0) return 15;
    pthread_join(t, &ret);
    pthread_attr_destroy(&attr);
    return (long)ret == 8 ? 42 : 16;
}
