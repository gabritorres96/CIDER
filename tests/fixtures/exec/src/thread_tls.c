/* Thread-local storage per thread: every thread starts from the template, nobody sees anybody else's copy. */
#include "darwin.h"

__thread int counter = 100;
__thread char name[16] = "template";
__thread long zeroed;

static void *worker(void *arg) {
    long id = (long)arg;
    if (counter != 100 || zeroed != 0 || strcmp(name, "template")) return (void *)-1;   /* fresh copy of the template */
    for (int i = 0; i < 5; ++i) counter += (int)id;                                       /* thread 1 adds 5, thread 2 adds 10 */
    name[0] = 'T';
    name[1] = (char)('0' + id);
    zeroed = id * 1000;
    sched_yield();                                                                        /* let the other thread modify ITS copy */
    if (name[1] != '0' + id || zeroed != id * 1000) return (void *)-2;
    return (void *)(long)counter;
}

int main(void) {
    counter += 1;                                  /* main's own copy: 101 */
    zeroed = 7;
    pthread_t a, b;
    void *ra = 0, *rb = 0;
    pthread_create(&a, 0, worker, (void *)1);
    pthread_create(&b, 0, worker, (void *)2);
    pthread_join(a, &ra);
    pthread_join(b, &rb);
    printf("a=%ld b=%ld main=%d %s %ld\n", (long)ra, (long)rb, counter, name, zeroed);
    if ((long)ra != 105 || (long)rb != 110) return 1;
    if (counter != 101 || zeroed != 7 || strcmp(name, "template")) return 2;   /* main untouched by the threads */
    return 42;
}
