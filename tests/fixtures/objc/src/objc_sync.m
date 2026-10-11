/* @synchronized (objc_sync_enter / objc_sync_exit) and associated objects, compiled by clang with manual reference counting.
   Modes (argv[1]): none/"ok" = every check, exit 42;  "bad_policy" = an undefined association policy (the run must end with a diagnostic);
   "unlock" = objc_sync_exit on an object this thread does not own, reported through the return value. */
#include "objc_min.h"
#include "darwin.h"

static int live, deallocs, copies;

__attribute__((objc_root_class))
@interface Obj {
    Class isa;
    int tag;
}
+ (id)newWithTag:(int)t;
- (id)retain;
- (oneway void)release;
- (id)autorelease;
- (id)copy;
- (int)tag;
- (void)dealloc;
@end

@implementation Obj
+ (id)newWithTag:(int)t { Obj *o = class_createInstance(self, 0); o->tag = t; ++live; return o; }
- (id)retain { return objc_retain(self); }
- (oneway void)release { objc_release(self); }
- (id)autorelease { return objc_autorelease(self); }
- (id)copy { ++copies; return [Obj newWithTag:tag + 100]; }                  /* +1 */
- (int)tag { return tag; }
- (void)dealloc { ++deallocs; --live; object_dispose(self); }
@end

/* ---- @synchronized ------------------------------------------------------------------------------------------------ */
static int shared;                     /* modified only inside the critical section */
static int inside;                     /* 1 while some thread is in the critical section */
static int violations;
static Obj *lock_obj;

static void *worker(void *arg) {
    (void)arg;
    for (int i = 0; i < 50; ++i) {
        objc_sync_enter(lock_obj);
        if (inside) ++violations;                         /* another thread is in the section: not exclusive */
        inside = 1;
        int v = shared;
        sched_yield();                                    /* hand control to the other thread INSIDE the section */
        shared = v + 1;
        inside = 0;
        objc_sync_exit(lock_obj);
        sched_yield();
    }
    return NULL;
}

static int sync_checks(void) {
    Obj *a = [Obj newWithTag:1], *b = [Obj newWithTag:2];
    lock_obj = [Obj newWithTag:3];
    if (objc_sync_enter(a) != 0) return 1;
    if (objc_sync_enter(a) != 0) return 2;                /* recursive */
    if (objc_sync_enter(b) != 0) return 3;                /* a second object is independent */
    if (objc_sync_exit(a) != 0 || objc_sync_exit(a) != 0) return 4;
    if (objc_sync_exit(a) != -1) return 5;                /* no longer locked: OBJC_SYNC_NOT_OWNING_THREAD_ERROR */
    if (objc_sync_exit(b) != 0) return 6;
    if (objc_sync_exit(b) != -1) return 7;
    if (objc_sync_enter(nil) != 0 || objc_sync_exit(nil) != 0) return 8;       /* @synchronized(nil) is a no-op */
    if (objc_sync_exit(lock_obj) != -1) return 9;
    pthread_t t1, t2, t3;                                 /* three threads: two of them can be waiting for the same lock at once */
    if (pthread_create(&t1, NULL, worker, NULL) || pthread_create(&t2, NULL, worker, NULL) || pthread_create(&t3, NULL, worker, NULL)) return 10;
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    pthread_join(t3, NULL);
    if (violations != 0) return 11;                       /* mutual exclusion held although both threads yielded inside the section */
    if (shared != 150) return 12;                         /* no lost update */
    if (objc_sync_exit(lock_obj) != -1) return 13;        /* every enter was matched */
    [a release]; [b release]; [lock_obj release];
    return 0;
}

/* ---- associated objects ------------------------------------------------------------------------------------------------ */
static char key1, key2, key3;

static int assoc_checks(void) {
    const int d0 = deallocs;                               /* dealloc counter at the start of this section */
    Obj *host = [Obj newWithTag:10];
    Obj *val = [Obj newWithTag:20];
    int base_live = live;                                  /* host + val */
    if (objc_getAssociatedObject(host, &key1) != nil) return 20;
    /* RETAIN: the association owns a reference */
    objc_setAssociatedObject(host, &key1, val, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [val release];                                         /* our reference goes: the association keeps it alive */
    if (live != base_live) return 21;
    if (objc_getAssociatedObject(host, &key1) != val) return 22;
    if ([(Obj *)objc_getAssociatedObject(host, &key1) tag] != 20) return 23;
    /* replacing releases the old value; assigning nil removes the association */
    Obj *val2 = [Obj newWithTag:21];
    objc_setAssociatedObject(host, &key1, val2, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    if (deallocs - d0 != 1) { printf("deallocs=%d live=%d\n", deallocs, live); return 24; }
    [val2 release];
    if (objc_getAssociatedObject(host, &key1) != val2) return 25;
    objc_setAssociatedObject(host, &key1, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    if (deallocs - d0 != 2) return 26;
    if (objc_getAssociatedObject(host, &key1) != nil) return 27;
    /* ASSIGN: no reference is taken */
    Obj *weak = [Obj newWithTag:30];
    objc_setAssociatedObject(host, &key2, weak, OBJC_ASSOCIATION_ASSIGN);
    if (objc_getAssociatedObject(host, &key2) != weak) return 28;
    [weak release];
    if (deallocs - d0 != 3) return 29;                          /* the association did not keep it alive */
    objc_setAssociatedObject(host, &key2, nil, OBJC_ASSOCIATION_ASSIGN);
    /* COPY: the association owns a copy, the original is untouched */
    Obj *orig = [Obj newWithTag:40];
    objc_setAssociatedObject(host, &key3, orig, OBJC_ASSOCIATION_COPY_NONATOMIC);
    if (copies != 1) return 30;
    Obj *got = objc_getAssociatedObject(host, &key3);
    if (got == orig || [got tag] != 140) return 31;
    [orig release];
    if (deallocs - d0 != 4) return 32;
    /* the host object's death releases what it still owns */
    Obj *extra = [Obj newWithTag:50];
    objc_setAssociatedObject(host, &key1, extra, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [extra release];
    int before = deallocs;
    [host release];
    if (deallocs != before + 3) return 33;                 /* host + the COPY value + extra */
    /* objc_removeAssociatedObjects */
    Obj *h2 = [Obj newWithTag:60], *v2 = [Obj newWithTag:61];
    objc_setAssociatedObject(h2, &key1, v2, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [v2 release];
    before = deallocs;
    objc_removeAssociatedObjects(h2);
    if (deallocs != before + 1) return 34;
    if (objc_getAssociatedObject(h2, &key1) != nil) return 35;
    [h2 release];
    /* nil object: nothing happens, nothing crashes */
    objc_setAssociatedObject(nil, &key1, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    if (objc_getAssociatedObject(nil, &key1) != nil) return 36;
    /* atomic RETAIN: the getter returns the value retained and autoreleased, so it survives the association being replaced */
    {
        Obj *h3 = [Obj newWithTag:70], *v3 = [Obj newWithTag:71];
        objc_setAssociatedObject(h3, &key1, v3, OBJC_ASSOCIATION_RETAIN);
        [v3 release];
        void *pool = objc_autoreleasePoolPush();
        Obj *held = objc_getAssociatedObject(h3, &key1);
        before = deallocs;
        objc_setAssociatedObject(h3, &key1, nil, OBJC_ASSOCIATION_RETAIN);
        if (deallocs != before) return 38;                 /* still referenced by the pool */
        if ([held tag] != 71) return 39;
        objc_autoreleasePoolPop(pool);
        if (deallocs != before + 1) return 40;              /* the pool drained: now it dies */
        [h3 release];
    }
    if (live != 0) return 37;
    return 0;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "ok";
    if (mode[0] == 'b') {                                 /* bad_policy */
        Obj *o = [Obj newWithTag:1];
        puts("before");
        objc_setAssociatedObject(o, &key1, o, 2);        /* 2 is not a policy */
        puts("not reached");
        return 1;
    }
    if (mode[0] == 'u') {                                 /* unlock: report what exit says */
        Obj *o = [Obj newWithTag:1];
        printf("exit=%d\n", objc_sync_exit(o));
        objc_sync_enter(o);
        printf("exit=%d\n", objc_sync_exit(o));
        printf("exit=%d\n", objc_sync_exit(o));
        return 0;
    }
    int bad = sync_checks();
    if (bad) { printf("sync check %d failed\n", bad); return bad; }
    bad = assoc_checks();
    if (bad) { printf("association check %d failed\n", bad); return bad; }
    puts("sync and associations ok");
    return 42;
}
