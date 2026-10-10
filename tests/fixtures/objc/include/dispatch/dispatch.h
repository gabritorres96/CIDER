/* Hand-written declarations of the libdispatch subset CIDER implements (see docs/LIBDISPATCH.md). Not Apple's <dispatch/dispatch.h>. */
#ifndef CIDER_DISPATCH_H
#define CIDER_DISPATCH_H
#include "../Foundation/Foundation.h"

@interface OS_dispatch_queue : NSObject
@end
@interface OS_dispatch_group : NSObject
@end
@interface OS_dispatch_semaphore : NSObject
@end
typedef OS_dispatch_queue *dispatch_queue_t;
typedef OS_dispatch_group *dispatch_group_t;
typedef OS_dispatch_semaphore *dispatch_semaphore_t;
typedef void (^dispatch_block_t)(void);
typedef unsigned long long dispatch_time_t;
typedef long dispatch_once_t;
typedef void (*dispatch_function_t)(void *);
typedef const void *dispatch_queue_attr_t;

#define DISPATCH_TIME_NOW 0ull
#define DISPATCH_TIME_FOREVER (~0ull)
#define NSEC_PER_SEC 1000000000ull
#define NSEC_PER_MSEC 1000000ull
#define DISPATCH_QUEUE_SERIAL ((dispatch_queue_attr_t)0)
#define DISPATCH_QUEUE_PRIORITY_HIGH 2
#define DISPATCH_QUEUE_PRIORITY_DEFAULT 0
#define DISPATCH_QUEUE_PRIORITY_LOW (-2)
#define DISPATCH_QUEUE_PRIORITY_BACKGROUND (-32768)

struct dispatch_queue_s { char opaque[16]; };
extern struct dispatch_queue_s _dispatch_main_q;
extern const struct dispatch_queue_s _dispatch_queue_attr_concurrent;
#define DISPATCH_QUEUE_CONCURRENT ((dispatch_queue_attr_t)&_dispatch_queue_attr_concurrent)
static inline dispatch_queue_t dispatch_get_main_queue(void) { return (__bridge dispatch_queue_t)(void *)&_dispatch_main_q; }

dispatch_queue_t dispatch_get_global_queue(long identifier, unsigned long flags);
dispatch_queue_t dispatch_queue_create(const char *label, dispatch_queue_attr_t attr) __attribute__((ns_returns_retained));
const char *dispatch_queue_get_label(dispatch_queue_t queue);
void dispatch_async(dispatch_queue_t queue, dispatch_block_t block);
void dispatch_async_f(dispatch_queue_t queue, void *context, dispatch_function_t work);
void dispatch_sync(dispatch_queue_t queue, dispatch_block_t block);
void dispatch_sync_f(dispatch_queue_t queue, void *context, dispatch_function_t work);
void dispatch_barrier_async(dispatch_queue_t queue, dispatch_block_t block);
void dispatch_barrier_sync(dispatch_queue_t queue, dispatch_block_t block);
void dispatch_after(dispatch_time_t when, dispatch_queue_t queue, dispatch_block_t block);
dispatch_time_t dispatch_time(dispatch_time_t when, long long delta);
void dispatch_once(dispatch_once_t *predicate, dispatch_block_t block);
void dispatch_apply(unsigned long iterations, dispatch_queue_t queue, void (^block)(unsigned long));
void dispatch_main(void) __attribute__((noreturn));

dispatch_group_t dispatch_group_create(void) __attribute__((ns_returns_retained));
void dispatch_group_enter(dispatch_group_t group);
void dispatch_group_leave(dispatch_group_t group);
void dispatch_group_async(dispatch_group_t group, dispatch_queue_t queue, dispatch_block_t block);
void dispatch_group_notify(dispatch_group_t group, dispatch_queue_t queue, dispatch_block_t block);
long dispatch_group_wait(dispatch_group_t group, dispatch_time_t timeout);
dispatch_semaphore_t dispatch_semaphore_create(long value) __attribute__((ns_returns_retained));
long dispatch_semaphore_signal(dispatch_semaphore_t dsema);
long dispatch_semaphore_wait(dispatch_semaphore_t dsema, dispatch_time_t timeout);

/* libSystem */
void exit(int);
void *pthread_self(void);
#endif
