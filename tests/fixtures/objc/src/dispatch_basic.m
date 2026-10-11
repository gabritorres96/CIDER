/* libdispatch subset from guest code (ARC): main queue, serial and global queues, sync/async, after, once, groups, semaphores, apply, a worker
   calling dispatch_sync on the main queue, and ObjC objects captured by blocks. The program is driven from the main queue (dispatch_main) and
   ends with exit(failed checks). */
#include "dispatch/dispatch.h"

static int fails;
#define CHECK(...) do { if (!(__VA_ARGS__)) { fails++; NSLog(@"FAIL line %d: %s", __LINE__, #__VA_ARGS__); } } while (0)

struct tv { long sec; int usec; };
int gettimeofday(struct tv *, void *);
static long long now_us(void) { struct tv t; gettimeofday(&t, 0); return (long long)t.sec * 1000000 + t.usec; }

static NSMutableArray *order;
static void *main_thread;
static void *worker_thread_seen;
static int apply_sum, once_count, once_target;
static dispatch_queue_t keep_alive_queue;

static void once_body(void) { once_count++; }
static void work_fn(void *ctx) { *(int *)ctx = 99; }

static void finish(void) {
    CHECK([order count] >= 1);
    NSLog(@"order: %@", [order componentsJoinedByString:@","]);
    NSLog(@"dispatch checks done: %d failures", fails);
    order = nil;                           /* strong globals are never released by the process: drop them so the leak counter means something */
    keep_alive_queue = nil;
    exit(fails);
}

static void phase_c(void) {               /* runs on the main queue, second hop */
    CHECK(pthread_self() == main_thread);
    long long t0 = now_us();
    __block long long fired_after = 0;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 40 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        fired_after = now_us() - t0;
        [order addObject:@"after"];
        CHECK(fired_after >= 35000);       /* not earlier than the delay (small slack for the clock) */
        CHECK(pthread_self() == main_thread);
    });
    dispatch_async(dispatch_get_main_queue(), ^{ [order addObject:@"immediate"]; });
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 120 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        NSUInteger ia = [order indexOfObject:@"after"], ii = [order indexOfObject:@"immediate"];
        CHECK(ii < ia);                    /* the immediate block ran before the delayed one although it was submitted later */
        CHECK([order containsObject:@"notified"]);
        CHECK([order containsObject:@"fromworker"]);
        finish();
    });
}

static void phase_b(void) {               /* runs on the main queue; the worker below needs the main thread to return to the loop */
    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        void *self_thread = pthread_self();
        CHECK(self_thread != main_thread);
        dispatch_sync(dispatch_get_main_queue(), ^{
            [order addObject:@"fromworker"];
            CHECK(pthread_self() == main_thread);       /* a main-queue block never runs on another thread */
        });
        dispatch_async(dispatch_get_main_queue(), ^{ phase_c(); });
    });
}

static void phase_a(void) {
    main_thread = pthread_self();

    /* serial queue: FIFO, a worker thread for async work, sync runs on the caller after earlier async blocks */
    dispatch_queue_t serial = dispatch_queue_create("com.orvyn.test.serial", DISPATCH_QUEUE_SERIAL);
    CHECK(serial != nil);
    CHECK(__builtin_strcmp(dispatch_queue_get_label(serial), "com.orvyn.test.serial") == 0);
    for (int i = 0; i < 3; i++) {
        NSString *tag = [NSString stringWithFormat:@"s%d", i];                 /* ObjC object captured by the block (retained by the copy) */
        dispatch_async(serial, ^{
            [order addObject:tag];
            if (i == 0) worker_thread_seen = pthread_self();
        });
    }
    dispatch_sync(serial, ^{ [order addObject:@"sync"]; CHECK(pthread_self() == main_thread); });

    /* semaphore + global queue */
    __block int v = 0;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_global_queue(0, 0), ^{ v = 42; dispatch_semaphore_signal(sem); });
    CHECK(dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER) == 0);
    CHECK(v == 42);
    CHECK(dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_MSEC)) != 0);      /* times out */

    /* group */
    dispatch_group_t group = dispatch_group_create();
    __block int counter = 0;
    for (int i = 0; i < 3; i++) dispatch_group_async(group, dispatch_get_global_queue(0, 0), ^{ counter++; });
    CHECK(dispatch_group_wait(group, DISPATCH_TIME_FOREVER) == 0);
    CHECK(counter == 3);
    dispatch_group_notify(group, dispatch_get_main_queue(), ^{ [order addObject:@"notified"]; });
    dispatch_group_enter(group);
    dispatch_group_leave(group);

    /* once, function forms, apply */
    static dispatch_once_t once;
    for (int i = 0; i < 3; i++) dispatch_once(&once, ^{ once_body(); });
    dispatch_async(dispatch_get_global_queue(0, 0), ^{ dispatch_once(&once, ^{ once_body(); }); });
    int via_f = 0;
    dispatch_sync_f(serial, &via_f, work_fn);
    CHECK(via_f == 99);
    dispatch_apply(5, dispatch_get_global_queue(0, 0), ^(unsigned long i) { apply_sum += (int)i; });
    CHECK(apply_sum == 10);

    /* order check of the serial queue */
    NSUInteger s0 = [order indexOfObject:@"s0"], s1 = [order indexOfObject:@"s1"], s2 = [order indexOfObject:@"s2"], sy = [order indexOfObject:@"sync"];
    CHECK(s0 < s1 && s1 < s2 && s2 < sy);
    CHECK(worker_thread_seen != 0 && worker_thread_seen != main_thread);
    CHECK(once_count == 1);
    keep_alive_queue = serial;

    dispatch_async(dispatch_get_main_queue(), ^{ phase_b(); });
}

int main(void) {
    @autoreleasepool {
        order = [NSMutableArray array];
        dispatch_async(dispatch_get_main_queue(), ^{ [order addObject:@"m1"]; });
        dispatch_async(dispatch_get_main_queue(), ^{ [order addObject:@"m2"]; });
        dispatch_async(dispatch_get_main_queue(), ^{
            CHECK([order count] == 2 && [order[0] isEqualToString:@"m1"] && [order[1] isEqualToString:@"m2"]);
            phase_a();
        });
    }
    dispatch_main();
}
