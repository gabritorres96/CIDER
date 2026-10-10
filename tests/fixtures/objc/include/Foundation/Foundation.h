/* Hand-written declarations of the Foundation subset CIDER implements (see docs/FOUNDATION.md). This is NOT Apple's Foundation.h: the
   Apple SDK is not available and is not redistributed; only the classes and selectors listed below exist in the runtime. */
#ifndef CIDER_FOUNDATION_H
#define CIDER_FOUNDATION_H
#include "../objc_min.h"
#include "../CoreGraphics/CoreGraphics.h"

typedef long NSInteger;
typedef unsigned long NSUInteger;
typedef struct _NSZone NSZone;
typedef struct { NSUInteger location, length; } NSRange;
typedef CGPoint NSPoint;
typedef CGSize NSSize;
typedef CGRect NSRect;
extern const NSPoint NSZeroPoint;
extern const NSSize NSZeroSize;
extern const NSRect NSZeroRect;
static inline NSPoint NSMakePoint(CGFloat x, CGFloat y) { return CGPointMake(x, y); }
static inline NSSize NSMakeSize(CGFloat w, CGFloat h) { return CGSizeMake(w, h); }
static inline NSRect NSMakeRect(CGFloat x, CGFloat y, CGFloat w, CGFloat h) { return CGRectMake(x, y, w, h); }
static inline CGFloat NSMinX(NSRect r) { return CGRectGetMinX(r); }
static inline CGFloat NSMaxX(NSRect r) { return CGRectGetMaxX(r); }
static inline CGFloat NSMinY(NSRect r) { return CGRectGetMinY(r); }
static inline CGFloat NSMaxY(NSRect r) { return CGRectGetMaxY(r); }
static inline CGFloat NSWidth(NSRect r) { return CGRectGetWidth(r); }
static inline CGFloat NSHeight(NSRect r) { return CGRectGetHeight(r); }
static inline NSRange NSMakeRange(NSUInteger location, NSUInteger length) { NSRange r = { location, length }; return r; }
typedef NSInteger NSComparisonResult;
enum { NSOrderedAscending = -1, NSOrderedSame = 0, NSOrderedDescending = 1 };
#define NSNotFound ((NSInteger)0x7fffffffffffffffL)
typedef struct { unsigned long state; __unsafe_unretained id *itemsPtr; unsigned long *mutationsPtr; unsigned long extra[5]; } NSFastEnumerationState;

@class NSString, NSNumber, NSArray, NSDictionary, NSData, NSDate, NSURL, NSError;

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
- (NSString *)lastPathComponent;
- (NSString *)pathExtension;
- (NSString *)stringByDeletingLastPathComponent;
- (NSString *)stringByAppendingPathComponent:(NSString *)component;
- (BOOL)isAbsolutePath;
@end

@interface NSMutableString : NSString
+ (instancetype)stringWithCapacity:(NSUInteger)capacity;
- (instancetype)initWithCapacity:(NSUInteger)capacity;
- (void)appendString:(NSString *)string;
- (void)appendFormat:(NSString *)format, ... __attribute__((format(__NSString__, 1, 2)));
- (void)setString:(NSString *)string;
- (void)insertString:(NSString *)string atIndex:(NSUInteger)index;
- (void)deleteCharactersInRange:(NSRange)range;
- (void)replaceCharactersInRange:(NSRange)range withString:(NSString *)string;
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

@interface NSData : NSObject <NSCopying, NSMutableCopying>
+ (instancetype)data;
+ (instancetype)dataWithBytes:(const void *)bytes length:(NSUInteger)length;
+ (instancetype)dataWithData:(NSData *)data;
- (instancetype)initWithBytes:(const void *)bytes length:(NSUInteger)length;
- (NSUInteger)length;
- (const void *)bytes;
- (void)getBytes:(void *)buffer length:(NSUInteger)length;
- (BOOL)isEqualToData:(NSData *)other;
- (NSData *)subdataWithRange:(NSRange)range;
@end
@interface NSMutableData : NSData
+ (instancetype)dataWithCapacity:(NSUInteger)capacity;
+ (instancetype)dataWithLength:(NSUInteger)length;
- (instancetype)initWithCapacity:(NSUInteger)capacity;
- (void *)mutableBytes;
- (void)appendBytes:(const void *)bytes length:(NSUInteger)length;
- (void)appendData:(NSData *)data;
- (void)setLength:(NSUInteger)length;
- (void)increaseLengthBy:(NSUInteger)extra;
- (void)replaceBytesInRange:(NSRange)range withBytes:(const void *)bytes;
@end

typedef double NSTimeInterval;
@interface NSDate : NSObject <NSCopying>
+ (instancetype)date;
+ (instancetype)dateWithTimeIntervalSince1970:(NSTimeInterval)seconds;
+ (instancetype)dateWithTimeIntervalSinceReferenceDate:(NSTimeInterval)seconds;
+ (instancetype)dateWithTimeIntervalSinceNow:(NSTimeInterval)seconds;
- (instancetype)initWithTimeIntervalSince1970:(NSTimeInterval)seconds;
- (NSTimeInterval)timeIntervalSince1970;
- (NSTimeInterval)timeIntervalSinceReferenceDate;
- (NSTimeInterval)timeIntervalSinceNow;
- (NSTimeInterval)timeIntervalSinceDate:(NSDate *)other;
- (NSDate *)dateByAddingTimeInterval:(NSTimeInterval)seconds;
- (NSComparisonResult)compare:(NSDate *)other;
- (BOOL)isEqualToDate:(NSDate *)other;
- (NSDate *)earlierDate:(NSDate *)other;
- (NSDate *)laterDate:(NSDate *)other;
@end

@interface NSURL : NSObject <NSCopying>
+ (instancetype)URLWithString:(NSString *)string;
+ (instancetype)fileURLWithPath:(NSString *)path;
- (NSString *)absoluteString;
- (NSString *)relativeString;
- (NSString *)path;
- (NSString *)relativePath;
- (NSString *)scheme;
- (NSString *)host;
- (NSString *)query;
- (BOOL)isFileURL;
- (NSString *)lastPathComponent;
- (NSString *)pathExtension;
- (NSURL *)URLByAppendingPathComponent:(NSString *)component;
@end

extern NSString *const NSLocalizedDescriptionKey;
extern NSString *const NSFilePathErrorKey;
extern NSString *const NSUnderlyingErrorKey;
extern NSString *const NSCocoaErrorDomain;
extern NSString *const NSPOSIXErrorDomain;
@interface NSError : NSObject <NSCopying>
+ (instancetype)errorWithDomain:(NSString *)domain code:(NSInteger)code userInfo:(NSDictionary *)userInfo;
- (instancetype)initWithDomain:(NSString *)domain code:(NSInteger)code userInfo:(NSDictionary *)userInfo;
- (NSString *)domain;
- (NSInteger)code;
- (NSDictionary *)userInfo;
- (NSString *)localizedDescription;
@end

@interface NSFileManager : NSObject
+ (NSFileManager *)defaultManager;
- (BOOL)fileExistsAtPath:(NSString *)path;
- (BOOL)fileExistsAtPath:(NSString *)path isDirectory:(BOOL *)isDirectory;
- (NSData *)contentsAtPath:(NSString *)path;
- (BOOL)createFileAtPath:(NSString *)path contents:(NSData *)data attributes:(NSDictionary *)attributes;
- (BOOL)removeItemAtPath:(NSString *)path error:(NSError **)error;
- (BOOL)createDirectoryAtPath:(NSString *)path withIntermediateDirectories:(BOOL)intermediate attributes:(NSDictionary *)attributes error:(NSError **)error;
- (NSArray *)contentsOfDirectoryAtPath:(NSString *)path error:(NSError **)error;
- (NSString *)currentDirectoryPath;
@end

@interface NSNotification : NSObject
+ (instancetype)notificationWithName:(NSString *)name object:(id)object;
+ (instancetype)notificationWithName:(NSString *)name object:(id)object userInfo:(NSDictionary *)userInfo;
- (NSString *)name;
- (id)object;
- (NSDictionary *)userInfo;
@end
@interface NSNotificationCenter : NSObject
+ (NSNotificationCenter *)defaultCenter;
- (void)addObserver:(id)observer selector:(SEL)selector name:(NSString *)name object:(id)object;
- (id)addObserverForName:(NSString *)name object:(id)object queue:(id)queue usingBlock:(void (^)(NSNotification *note))block;
- (void)removeObserver:(id)observer;
- (void)postNotification:(NSNotification *)notification;
- (void)postNotificationName:(NSString *)name object:(id)object;
- (void)postNotificationName:(NSString *)name object:(id)object userInfo:(NSDictionary *)userInfo;
@end

@interface NSAutoreleasePool : NSObject
- (void)drain;
@end

BOOL NSPointInRect(NSPoint p, NSRect r);
BOOL NSMouseInRect(NSPoint p, NSRect r, BOOL flipped);
BOOL NSEqualRects(NSRect a, NSRect b); BOOL NSEqualPoints(NSPoint a, NSPoint b); BOOL NSEqualSizes(NSSize a, NSSize b);
BOOL NSIsEmptyRect(NSRect r); BOOL NSContainsRect(NSRect a, NSRect b);
NSRect NSIntersectionRect(NSRect a, NSRect b); NSRect NSUnionRect(NSRect a, NSRect b);
NSRect NSInsetRect(NSRect r, CGFloat dx, CGFloat dy); NSRect NSOffsetRect(NSRect r, CGFloat dx, CGFloat dy); NSRect NSIntegralRect(NSRect r);
NSString *NSStringFromRect(NSRect r); NSString *NSStringFromPoint(NSPoint p); NSString *NSStringFromSize(NSSize s);
extern NSString *const NSDefaultRunLoopMode;
extern NSString *const NSRunLoopCommonModes;
void NSLog(NSString *format, ...) __attribute__((format(__NSString__, 1, 2)));
NSString *NSStringFromClass(Class cls);
Class NSClassFromString(NSString *name);
NSString *NSStringFromSelector(SEL selector);
SEL NSSelectorFromString(NSString *name);

#endif
