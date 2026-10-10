/* Hand-written declarations of the Foundation subset CIDER implements (see docs/FOUNDATION.md). This is NOT Apple's Foundation.h: the
   Apple SDK is not available and is not redistributed; only the classes and selectors listed below exist in the runtime. */
#ifndef CIDER_FOUNDATION_H
#define CIDER_FOUNDATION_H
#include "../objc_min.h"

typedef long NSInteger;
typedef unsigned long NSUInteger;
typedef struct _NSZone NSZone;
typedef struct { NSUInteger location, length; } NSRange;
static inline NSRange NSMakeRange(NSUInteger location, NSUInteger length) { NSRange r = { location, length }; return r; }
typedef NSInteger NSComparisonResult;
enum { NSOrderedAscending = -1, NSOrderedSame = 0, NSOrderedDescending = 1 };
#define NSNotFound ((NSInteger)0x7fffffffffffffffL)
typedef struct { unsigned long state; __unsafe_unretained id *itemsPtr; unsigned long *mutationsPtr; unsigned long extra[5]; } NSFastEnumerationState;

@class NSString, NSNumber, NSArray, NSDictionary;

@protocol NSCopying
- (id)copyWithZone:(NSZone *)zone;
@end
@protocol NSMutableCopying
- (id)mutableCopyWithZone:(NSZone *)zone;
@end
@protocol NSFastEnumeration
- (NSUInteger)countByEnumeratingWithState:(NSFastEnumerationState *)state objects:(__unsafe_unretained id *)buffer count:(NSUInteger)len;
@end

__attribute__((objc_root_class))
@interface NSObject {
    Class isa;
}
+ (instancetype)alloc;
+ (instancetype)new;
+ (Class)class;
+ (Class)superclass;
+ (BOOL)instancesRespondToSelector:(SEL)selector;
+ (BOOL)isSubclassOfClass:(Class)cls;
+ (NSString *)description;
- (instancetype)init;
- (void)dealloc;
- (id)retain;
- (oneway void)release;
- (id)autorelease;
- (NSUInteger)retainCount;
- (Class)class;
- (id)self;
- (BOOL)isEqual:(id)object;
- (NSUInteger)hash;
- (BOOL)isKindOfClass:(Class)cls;
- (BOOL)isMemberOfClass:(Class)cls;
- (BOOL)respondsToSelector:(SEL)selector;
- (BOOL)conformsToProtocol:(Protocol *)protocol;
- (NSString *)description;
- (NSString *)debugDescription;
- (id)performSelector:(SEL)selector;
- (id)performSelector:(SEL)selector withObject:(id)object;
- (id)copy;
- (id)mutableCopy;
@end

@interface NSString : NSObject <NSCopying>
+ (instancetype)string;
+ (instancetype)stringWithUTF8String:(const char *)cstr;
+ (instancetype)stringWithString:(NSString *)string;
+ (instancetype)stringWithFormat:(NSString *)format, ... __attribute__((format(__NSString__, 1, 2)));
- (instancetype)initWithUTF8String:(const char *)cstr;
- (instancetype)initWithString:(NSString *)string;
- (instancetype)initWithFormat:(NSString *)format, ... __attribute__((format(__NSString__, 1, 2)));
- (NSUInteger)length;
- (const char *)UTF8String;
- (unsigned short)characterAtIndex:(NSUInteger)index;
- (BOOL)isEqualToString:(NSString *)other;
- (NSComparisonResult)compare:(NSString *)other;
- (BOOL)hasPrefix:(NSString *)prefix;
- (BOOL)hasSuffix:(NSString *)suffix;
- (BOOL)containsString:(NSString *)other;
- (NSRange)rangeOfString:(NSString *)other;
- (int)intValue;
- (NSInteger)integerValue;
- (double)doubleValue;
- (NSString *)stringByAppendingString:(NSString *)other;
- (NSString *)substringToIndex:(NSUInteger)index;
- (NSString *)substringFromIndex:(NSUInteger)index;
- (NSString *)substringWithRange:(NSRange)range;
- (NSString *)lowercaseString;
- (NSString *)uppercaseString;
- (NSArray *)componentsSeparatedByString:(NSString *)separator;
@end

@interface NSNumber : NSObject <NSCopying>
+ (NSNumber *)numberWithInt:(int)value;
+ (NSNumber *)numberWithInteger:(NSInteger)value;
+ (NSNumber *)numberWithLong:(long)value;
+ (NSNumber *)numberWithLongLong:(long long)value;
+ (NSNumber *)numberWithUnsignedInt:(unsigned int)value;
+ (NSNumber *)numberWithUnsignedInteger:(NSUInteger)value;
+ (NSNumber *)numberWithUnsignedLong:(unsigned long)value;
+ (NSNumber *)numberWithUnsignedLongLong:(unsigned long long)value;
+ (NSNumber *)numberWithChar:(signed char)value;
+ (NSNumber *)numberWithUnsignedChar:(unsigned char)value;
+ (NSNumber *)numberWithShort:(short)value;
+ (NSNumber *)numberWithBool:(BOOL)value;
+ (NSNumber *)numberWithDouble:(double)value;
+ (NSNumber *)numberWithFloat:(float)value;
- (int)intValue;
- (NSInteger)integerValue;
- (long long)longLongValue;
- (BOOL)boolValue;
- (double)doubleValue;
- (float)floatValue;
- (BOOL)isEqualToNumber:(NSNumber *)other;
- (NSComparisonResult)compare:(NSNumber *)other;
- (NSString *)stringValue;
@end

@interface NSArray : NSObject <NSCopying, NSMutableCopying, NSFastEnumeration>
+ (instancetype)array;
+ (instancetype)arrayWithObject:(id)object;
+ (instancetype)arrayWithObjects:(const id [])objects count:(NSUInteger)count;
+ (instancetype)arrayWithObjects:(id)first, ... __attribute__((sentinel));
+ (instancetype)arrayWithArray:(NSArray *)array;
- (instancetype)initWithObjects:(const id [])objects count:(NSUInteger)count;
- (instancetype)initWithObjects:(id)first, ... __attribute__((sentinel));
- (NSUInteger)count;
- (id)objectAtIndex:(NSUInteger)index;
- (id)objectAtIndexedSubscript:(NSUInteger)index;
- (id)firstObject;
- (id)lastObject;
- (NSUInteger)indexOfObject:(id)object;
- (BOOL)containsObject:(id)object;
- (BOOL)isEqualToArray:(NSArray *)other;
- (NSString *)componentsJoinedByString:(NSString *)separator;
- (NSArray *)arrayByAddingObject:(id)object;
- (void)enumerateObjectsUsingBlock:(void (^)(id obj, NSUInteger idx, BOOL *stop))block;
- (NSUInteger)indexOfObjectPassingTest:(BOOL (^)(id obj, NSUInteger idx, BOOL *stop))predicate;
@end

@interface NSMutableArray : NSArray
+ (instancetype)arrayWithCapacity:(NSUInteger)capacity;
- (instancetype)initWithCapacity:(NSUInteger)capacity;
- (void)addObject:(id)object;
- (void)insertObject:(id)object atIndex:(NSUInteger)index;
- (void)removeObjectAtIndex:(NSUInteger)index;
- (void)removeLastObject;
- (void)removeAllObjects;
- (void)removeObject:(id)object;
- (void)replaceObjectAtIndex:(NSUInteger)index withObject:(id)object;
- (void)setObject:(id)object atIndexedSubscript:(NSUInteger)index;
@end

@interface NSDictionary : NSObject <NSCopying, NSMutableCopying, NSFastEnumeration>
+ (instancetype)dictionary;
+ (instancetype)dictionaryWithObject:(id)object forKey:(id<NSCopying>)key;
+ (instancetype)dictionaryWithObjects:(const id [])objects forKeys:(const id<NSCopying> [])keys count:(NSUInteger)count;
+ (instancetype)dictionaryWithDictionary:(NSDictionary *)dictionary;
- (instancetype)initWithObjects:(const id [])objects forKeys:(const id<NSCopying> [])keys count:(NSUInteger)count;
- (NSUInteger)count;
- (id)objectForKey:(id)key;
- (id)objectForKeyedSubscript:(id)key;
- (NSArray *)allKeys;
- (NSArray *)allValues;
- (BOOL)isEqualToDictionary:(NSDictionary *)other;
- (void)enumerateKeysAndObjectsUsingBlock:(void (^)(id key, id obj, BOOL *stop))block;
@end

@interface NSMutableDictionary : NSDictionary
+ (instancetype)dictionaryWithCapacity:(NSUInteger)capacity;
- (instancetype)initWithCapacity:(NSUInteger)capacity;
- (void)setObject:(id)object forKey:(id<NSCopying>)key;
- (void)setObject:(id)object forKeyedSubscript:(id<NSCopying>)key;
- (void)removeObjectForKey:(id)key;
- (void)removeAllObjects;
@end

@interface NSAutoreleasePool : NSObject
- (void)drain;
@end

void NSLog(NSString *format, ...) __attribute__((format(__NSString__, 1, 2)));
NSString *NSStringFromClass(Class cls);
Class NSClassFromString(NSString *name);
NSString *NSStringFromSelector(SEL selector);
SEL NSSelectorFromString(NSString *name);

#endif
