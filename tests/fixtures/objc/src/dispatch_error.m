/* libdispatch misuse that must end the guest with a diagnosis (libdispatch crashes or deadlocks in these cases).
   argv[1]: 's' dispatch_sync onto the main queue from the main thread, 'q' dispatch_sync onto the serial queue a block is running on,
            'b' NULL block, 'o' dispatch_once with a bad predicate, 'g' unbalanced dispatch_group_leave, 'x' exit() from a worker block. */
#include "dispatch/dispatch.h"

int main(int argc, char **argv) {
    char mode = argc > 1 ? argv[1][0] : 's';
    NSLog(@"before");
    switch (mode) {
        case 's': dispatch_sync(dispatch_get_main_queue(), ^{ NSLog(@"never"); }); break;
        case 'q': {
            dispatch_queue_t q = dispatch_queue_create("orvyn.q", DISPATCH_QUEUE_SERIAL);
            dispatch_sync(q, ^{ dispatch_sync(q, ^{ NSLog(@"never"); }); });
            break;
        }
        case 'b': dispatch_async(dispatch_get_main_queue(), (dispatch_block_t)0); break;
        case 'o': dispatch_once((dispatch_once_t *)16, ^{ NSLog(@"never"); }); break;
        case 'g': dispatch_group_leave(dispatch_group_create()); break;
        case 'x': {
            dispatch_queue_t q = dispatch_queue_create("orvyn.x", DISPATCH_QUEUE_SERIAL);
            dispatch_async(q, ^{ exit(33); });
            dispatch_semaphore_wait(dispatch_semaphore_create(0), DISPATCH_TIME_FOREVER);     /* never signalled: only exit() ends the process */
            break;
        }
    }
    NSLog(@"after");
    return 5;
}
