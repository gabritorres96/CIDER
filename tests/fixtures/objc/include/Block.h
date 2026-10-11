/* Hand-written declarations of the Blocks runtime entry points clang calls or documents (Apple's Block.h is not redistributed). */
#ifndef ORVYN_BLOCK_H
#define ORVYN_BLOCK_H
void *_Block_copy(const void *block);
void _Block_release(const void *block);
#define Block_copy(...) ((__typeof(__VA_ARGS__))_Block_copy((const void *)(__VA_ARGS__)))
#define Block_release(...) _Block_release((const void *)(__VA_ARGS__))
#endif
