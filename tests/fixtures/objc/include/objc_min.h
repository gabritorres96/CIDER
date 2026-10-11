/* Minimal hand-written declarations of the libobjc API used by the fixtures (the Apple SDK is not available and is not redistributed).
   Only prototypes: the behaviour comes from ORVYN's runtime. */
#ifndef ORVYN_OBJC_MIN_H
#define ORVYN_OBJC_MIN_H

typedef struct objc_class *Class;
typedef struct objc_object { Class isa; } *id;
typedef struct objc_selector *SEL;
typedef id (*IMP)(id, SEL, ...);
typedef struct objc_method *Method;
typedef struct objc_ivar *Ivar;
typedef signed char BOOL;
#define YES ((BOOL)1)
#define NO ((BOOL)0)
#define nil ((id)0)
#define Nil ((Class)0)
#ifndef NULL
#define NULL ((void *)0)
#endif

id objc_msgSend(id, SEL, ...);
Class objc_getClass(const char *);
Class objc_lookUpClass(const char *);
Class objc_getMetaClass(const char *);
SEL sel_registerName(const char *);
const char *sel_getName(SEL);
Class object_getClass(id);
const char *object_getClassName(id);
const char *class_getName(Class);
Class class_getSuperclass(Class);
BOOL class_isMetaClass(Class);
unsigned long class_getInstanceSize(Class);
/* the object comes back with retain count 1: under ARC the compiler must be told (otherwise it retains the +0 result of a C function) */
id class_createInstance(Class, unsigned long) __attribute__((ns_returns_retained));
id object_dispose(id);
Method class_getInstanceMethod(Class, SEL);
Method class_getClassMethod(Class, SEL);
IMP class_getMethodImplementation(Class, SEL);
BOOL class_respondsToSelector(Class, SEL);
IMP method_getImplementation(Method);
SEL method_getName(Method);
const char *method_getTypeEncoding(Method);
IMP method_setImplementation(Method, IMP);
void method_exchangeImplementations(Method, Method);
BOOL class_addMethod(Class, SEL, IMP, const char *);
IMP class_replaceMethod(Class, SEL, IMP, const char *);
Ivar class_getInstanceVariable(Class, const char *);
long ivar_getOffset(Ivar);
const char *ivar_getName(Ivar);
id object_getIvar(id, Ivar);
void object_setIvar(id, Ivar, id);
BOOL class_addIvar(Class, const char *, unsigned long, unsigned char, const char *);
Class objc_allocateClassPair(Class, const char *, unsigned long);
void objc_registerClassPair(Class);
void objc_disposeClassPair(Class);
Protocol *objc_getProtocol(const char *);
const char *protocol_getName(Protocol *);
BOOL class_conformsToProtocol(Class, Protocol *);

/* ARC / memory management entry points (also emitted by the compiler) */
id objc_retain(id);
void objc_release(id);
id objc_autorelease(id);
void *objc_autoreleasePoolPush(void);
void objc_autoreleasePoolPop(void *);
/* @synchronized and associated objects */
int objc_sync_enter(id);
int objc_sync_exit(id);
typedef unsigned long objc_AssociationPolicy;
#define OBJC_ASSOCIATION_ASSIGN 0
#define OBJC_ASSOCIATION_RETAIN_NONATOMIC 1
#define OBJC_ASSOCIATION_COPY_NONATOMIC 3
#define OBJC_ASSOCIATION_RETAIN 01401
#define OBJC_ASSOCIATION_COPY 01403
void objc_setAssociatedObject(id, const void *, id, objc_AssociationPolicy);
id objc_getAssociatedObject(id, const void *);
void objc_removeAssociatedObjects(id);
id objc_storeWeak(id *, id);
id objc_loadWeak(id *);
void objc_destroyWeak(id *);
#endif
