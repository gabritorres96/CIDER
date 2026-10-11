/* Hand-written declarations of the CoreFoundation subset ORVYN implements (see docs/FOUNDATION.md, CoreFoundation). Not the Apple header. */
#ifndef ORVYN_COREFOUNDATION_H
#define ORVYN_COREFOUNDATION_H
#include "../objc_min.h"
typedef const void *CFTypeRef;
typedef const struct __CFAllocator *CFAllocatorRef;
typedef const struct __CFString *CFStringRef;
typedef struct __CFString *CFMutableStringRef;
typedef const struct __CFArray *CFArrayRef;
typedef struct __CFArray *CFMutableArrayRef;
typedef const struct __CFDictionary *CFDictionaryRef;
typedef struct __CFDictionary *CFMutableDictionaryRef;
typedef const struct __CFNumber *CFNumberRef;
typedef struct __CFRunLoop *CFRunLoopRef;
typedef struct __CFRunLoopSource *CFRunLoopSourceRef;
typedef struct __CFRunLoopTimer *CFRunLoopTimerRef;
typedef const struct __CFString *CFRunLoopMode;
typedef unsigned long CFTypeID;
typedef long CFIndex;
typedef unsigned char Boolean;
typedef unsigned int CFStringEncoding;
typedef double CFTimeInterval;
typedef double CFAbsoluteTime;
typedef int CFComparisonResult;
typedef int CFNumberType;
typedef int SInt32;
enum { kCFStringEncodingMacRoman = 0, kCFStringEncodingASCII = 0x0600, kCFStringEncodingISOLatin1 = 0x0201, kCFStringEncodingUTF8 = 0x08000100 };
enum { kCFCompareCaseInsensitive = 1 };
enum { kCFNumberSInt32Type = 3, kCFNumberSInt64Type = 4, kCFNumberFloat64Type = 6, kCFNumberIntType = 9, kCFNumberLongType = 10, kCFNumberDoubleType = 13 };
enum { kCFRunLoopRunFinished = 1, kCFRunLoopRunStopped = 2, kCFRunLoopRunTimedOut = 3, kCFRunLoopRunHandledSource = 4 };

typedef struct { CFIndex version; const void *retain, *release, *copyDescription, *equal; } CFArrayCallBacks;
typedef struct { CFIndex version; const void *retain, *release, *copyDescription, *equal, *hash; } CFDictionaryKeyCallBacks;
typedef struct { CFIndex version; const void *retain, *release, *copyDescription, *equal; } CFDictionaryValueCallBacks;
typedef struct {
    CFIndex version; void *info; const void *retain, *release, *copyDescription, *equal, *hash;
    void (*schedule)(void *info, CFRunLoopRef rl, CFRunLoopMode mode);
    void (*cancel)(void *info, CFRunLoopRef rl, CFRunLoopMode mode);
    void (*perform)(void *info);
} CFRunLoopSourceContext;
typedef struct { CFIndex version; void *info; const void *retain, *release, *copyDescription; } CFRunLoopTimerContext;
typedef void (*CFRunLoopTimerCallBack)(CFRunLoopTimerRef timer, void *info);

extern const CFArrayCallBacks kCFTypeArrayCallBacks;
extern const CFDictionaryKeyCallBacks kCFTypeDictionaryKeyCallBacks;
extern const CFDictionaryValueCallBacks kCFTypeDictionaryValueCallBacks;
extern const CFAllocatorRef kCFAllocatorDefault;
extern const CFRunLoopMode kCFRunLoopDefaultMode, kCFRunLoopCommonModes;

CFTypeRef CFRetain(CFTypeRef cf);
void CFRelease(CFTypeRef cf);
CFIndex CFGetRetainCount(CFTypeRef cf);
CFTypeID CFGetTypeID(CFTypeRef cf);
Boolean CFEqual(CFTypeRef a, CFTypeRef b);
unsigned long CFHash(CFTypeRef cf);
CFStringRef CFCopyDescription(CFTypeRef cf);
void CFShow(CFTypeRef cf);
CFTypeID CFStringGetTypeID(void); CFTypeID CFArrayGetTypeID(void); CFTypeID CFDictionaryGetTypeID(void); CFTypeID CFNumberGetTypeID(void);
CFStringRef CFStringCreateWithCString(CFAllocatorRef alloc, const char *cstr, CFStringEncoding enc);
CFStringRef CFStringCreateWithFormat(CFAllocatorRef alloc, CFDictionaryRef options, CFStringRef format, ...);
CFIndex CFStringGetLength(CFStringRef s);
unsigned short CFStringGetCharacterAtIndex(CFStringRef s, CFIndex idx);
Boolean CFStringGetCString(CFStringRef s, char *buffer, CFIndex size, CFStringEncoding enc);
const char *CFStringGetCStringPtr(CFStringRef s, CFStringEncoding enc);
CFComparisonResult CFStringCompare(CFStringRef a, CFStringRef b, unsigned long flags);
CFStringRef CFStringCreateCopy(CFAllocatorRef alloc, CFStringRef s);
CFMutableStringRef CFStringCreateMutable(CFAllocatorRef alloc, CFIndex maxLength);
void CFStringAppend(CFMutableStringRef s, CFStringRef t);
void CFStringAppendCString(CFMutableStringRef s, const char *cstr, CFStringEncoding enc);
CFArrayRef CFArrayCreate(CFAllocatorRef alloc, const void **values, CFIndex n, const CFArrayCallBacks *cb);
CFMutableArrayRef CFArrayCreateMutable(CFAllocatorRef alloc, CFIndex cap, const CFArrayCallBacks *cb);
CFIndex CFArrayGetCount(CFArrayRef a);
const void *CFArrayGetValueAtIndex(CFArrayRef a, CFIndex i);
void CFArrayAppendValue(CFMutableArrayRef a, const void *v);
void CFArrayRemoveValueAtIndex(CFMutableArrayRef a, CFIndex i);
CFDictionaryRef CFDictionaryCreate(CFAllocatorRef alloc, const void **keys, const void **values, CFIndex n, const CFDictionaryKeyCallBacks *kcb, const CFDictionaryValueCallBacks *vcb);
CFMutableDictionaryRef CFDictionaryCreateMutable(CFAllocatorRef alloc, CFIndex cap, const CFDictionaryKeyCallBacks *kcb, const CFDictionaryValueCallBacks *vcb);
CFIndex CFDictionaryGetCount(CFDictionaryRef d);
const void *CFDictionaryGetValue(CFDictionaryRef d, const void *key);
Boolean CFDictionaryContainsKey(CFDictionaryRef d, const void *key);
void CFDictionarySetValue(CFMutableDictionaryRef d, const void *key, const void *value);
void CFDictionaryAddValue(CFMutableDictionaryRef d, const void *key, const void *value);
void CFDictionaryRemoveValue(CFMutableDictionaryRef d, const void *key);
void CFDictionaryGetKeysAndValues(CFDictionaryRef d, const void **keys, const void **values);
CFNumberRef CFNumberCreate(CFAllocatorRef alloc, CFNumberType type, const void *value);
Boolean CFNumberGetValue(CFNumberRef n, CFNumberType type, void *value);
CFAbsoluteTime CFAbsoluteTimeGetCurrent(void);
CFRunLoopRef CFRunLoopGetMain(void); CFRunLoopRef CFRunLoopGetCurrent(void);
void CFRunLoopRun(void);
SInt32 CFRunLoopRunInMode(CFRunLoopMode mode, CFTimeInterval seconds, Boolean returnAfterSourceHandled);
void CFRunLoopStop(CFRunLoopRef rl); void CFRunLoopWakeUp(CFRunLoopRef rl);
CFRunLoopSourceRef CFRunLoopSourceCreate(CFAllocatorRef alloc, CFIndex order, CFRunLoopSourceContext *context);
void CFRunLoopSourceSignal(CFRunLoopSourceRef s); Boolean CFRunLoopSourceIsValid(CFRunLoopSourceRef s); void CFRunLoopSourceInvalidate(CFRunLoopSourceRef s);
void CFRunLoopAddSource(CFRunLoopRef rl, CFRunLoopSourceRef s, CFRunLoopMode mode);
void CFRunLoopRemoveSource(CFRunLoopRef rl, CFRunLoopSourceRef s, CFRunLoopMode mode);
Boolean CFRunLoopContainsSource(CFRunLoopRef rl, CFRunLoopSourceRef s, CFRunLoopMode mode);
CFRunLoopTimerRef CFRunLoopTimerCreate(CFAllocatorRef alloc, CFAbsoluteTime fireDate, CFTimeInterval interval, unsigned long flags, CFIndex order, CFRunLoopTimerCallBack callout, CFRunLoopTimerContext *context);
void CFRunLoopAddTimer(CFRunLoopRef rl, CFRunLoopTimerRef t, CFRunLoopMode mode);
void CFRunLoopRemoveTimer(CFRunLoopRef rl, CFRunLoopTimerRef t, CFRunLoopMode mode);
void CFRunLoopTimerInvalidate(CFRunLoopTimerRef t);
Boolean CFRunLoopTimerIsValid(CFRunLoopTimerRef t);
#define CFSTR(x) ((CFStringRef) __builtin___CFStringMakeConstantString("" x ""))
#endif
