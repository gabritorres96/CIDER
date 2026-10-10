/* Process-wide exit semantics. argv[1]: 'x' exit() from a non-main thread while main waits in pthread_join
   'm' main returns while another thread is still running; 'l' too many threads; 'f' a thread faults. */
#include "darwin.h"

static void *sleeper(void *arg) {
    for (;;) usleep(1000);                 /* never returns by itself */
    return 0;
}
static void *quitter(void *arg) {
    usleep(5000);
    puts("thread calls exit(7)");
    exit(7);
    return 0;
}
static void *crash(void *arg) {
    *(volatile int *)0 = 1;
    return 0;
}
static void *brief(void *arg) { return arg; }

int main(int argc, char **argv) {
    char mode = argc > 1 ? argv[1][0] : 'm';
    pthread_t a, b;
    if (mode == 'x') {
        pthread_create(&a, 0, sleeper, 0);
        pthread_create(&b, 0, quitter, 0);
        pthread_join(a, 0);                /* blocks forever unless exit() in the other thread ends the process */
        return 99;
    }
    if (mode == 'f') {
        pthread_create(&a, 0, crash, 0);
        pthread_join(a, 0);
        return 98;
    }
    if (mode == 'l') {
        pthread_t t[200];
        int created = 0, err = 0;
        for (int i = 0; i < 200; ++i) {
            err = pthread_create(&t[i], 0, sleeper, 0);
            if (err) break;
            ++created;
        }
        printf("created=%d err=%d\n", created, err);
        return err == EAGAIN ? 40 + (created > 60 && created <= 64) * 2 : 1;
    }
    pthread_create(&a, 0, sleeper, 0);
    pthread_create(&b, 0, sleeper, 0);
    sched_yield();
    pthread_detach(b);
    puts("main returns");
    return 5;                               /* the sleeping threads must not keep the process alive */
}
