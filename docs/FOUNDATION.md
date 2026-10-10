# Foundation subset (CIDER 0.4)

CIDER implements a **small, host-written subset of Foundation** so that Objective-C programs that use strings, numbers, arrays,
dictionaries, `NSLog` and autorelease pools can run. It is not Apple's Foundation: no Apple code or framework binary is used or copied
(`src/foundation/` is written from the public documented behaviour of the classes), and it makes **no claim of Foundation, Cocoa or
macOS compatibility** beyond the classes and selectors listed here.

A program is linked against the subset when it has `LC_LOAD_DYLIB` for `/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation`
(or CoreFoundation). The runtime then logs `[CIDER][OBJC] Foundation compatibility loaded`. The linker resolves symbols per library: a
Foundation function or class that is not in the tables below fails the link with the symbol name (exit code 12) — it is never stubbed.

## How it works

* Each class is registered in the Objective-C runtime as a *native* class (`Runtime::declare_native_class` + `add_native_methods`). Native methods are C++
  functions behind System V→Windows thunks (or raw `GuestRegs` handlers for `double` results, `NSRange` returns, variadics and more than four arguments).
  Guest code sends messages with the ordinary `objc_msgSend`, so it cannot tell them apart from compiled methods, and guest classes can subclass them.
* Instances are normal Objective-C objects: `[isa][payload id]` followed by the instance variables of a guest subclass. The payload (UTF-8 text,
  number, element vector, entries) lives in a host table and is released by `-dealloc`.
* Strings are stored as UTF-8; lengths and indexes are UTF-16 code units, as in Apple's NSString. Compiler literals (`@"..."`) are read in place from the
  `__NSConstantString` structure the compiler emitted (`___CFConstantStringClassReference`); non-ASCII literals are UTF-16 in the binary and are converted once.
* `@[]` and `@{}` compile to loads of `___NSArray0__` / `___NSDictionary0__`; the runtime provides immortal empty collections for them.
* Errors that raise an `NSException` on macOS (range errors, nil arguments, wrong argument class) **end the guest** with
  `*** Terminating app due to uncaught exception '<Name>', reason: '…'`. `@try/@catch` do not exist (see [OBJECTIVE-C-RUNTIME.md](OBJECTIVE-C-RUNTIME.md)).
* Memory follows Cocoa rules: `+alloc` returns +1, factory methods return autoreleased objects, collections retain elements, dictionaries copy keys.
  The leak counter `objc_live_objects` in the run result is 0 for every Foundation fixture.

## Free functions (exported from Foundation)

| Symbol | Behaviour |
|---|---|
| `NSLog(NSString *format, ...)` | Formats with `%@` (calls `-description`), the printf conversions of the libSystem formatter, appends `\n` and writes to **stderr**. Apple's NSLog also writes a timestamp, process name, pid and thread id; **CIDER writes only the message** (documented deviation). `%n` is refused. |
| `NSStringFromClass`, `NSClassFromString`, `NSStringFromSelector`, `NSSelectorFromString` | As documented. |

Data symbols: `___CFConstantStringClassReference`, `___NSArray0__`, `___NSDictionary0__`.

## NSObject

Class methods: `alloc`, `allocWithZone:`, `new`, `class`, `superclass`, `initialize`, `instancesRespondToSelector:`, `isSubclassOfClass:`, `description`.
Instance methods: `init`, `dealloc`, `retain`, `release`, `autorelease`, `retainCount`, `class`, `self`, `isEqual:`, `hash`, `isKindOfClass:`,
`isMemberOfClass:`, `respondsToSelector:`, `conformsToProtocol:`, `description`, `debugDescription`, `performSelector:`, `performSelector:withObject:`,
`copy`, `mutableCopy` (both send `copyWithZone:` / `mutableCopyWithZone:` and trap when the class lacks it), `isProxy`.
`retainCount` of an immortal object (constant string) is `NSUIntegerMax`.

## NSString / NSConstantString

`+string`, `+stringWithUTF8String:` (nil on invalid UTF-8), `+stringWithString:`, `+stringWithFormat:`, `-init`, `-initWithUTF8String:`, `-initWithString:`,
`-initWithFormat:`, `-length`, `-UTF8String`, `-characterAtIndex:`, `-isEqualToString:`, `-isEqual:`, `-hash`, `-compare:`, `-hasPrefix:`, `-hasSuffix:`,
`-containsString:`, `-rangeOfString:` (returns `{NSNotFound, 0}` when absent), `-intValue`, `-integerValue`, `-doubleValue`, `-stringByAppendingString:`,
`-substringToIndex:`, `-substringFromIndex:`, `-substringWithRange:`, `-lowercaseString`, `-uppercaseString` (ASCII only), `-componentsSeparatedByString:`,
`-description`, `-copyWithZone:` (retain: immutable).
Not implemented: `NSMutableString`, encodings other than UTF-8, localized/case-insensitive comparison, regular expressions, paths, URLs, `NSAttributedString`.

## NSNumber

Constructors: `+numberWithInt:`, `Integer:`, `Long:`, `LongLong:`, `UnsignedInt:`, `UnsignedInteger:`, `UnsignedLong:`, `UnsignedLongLong:`, `Char:`,
`UnsignedChar:`, `Short:`, `Bool:`, `Double:`, `Float:` (everything `@42`, `@3.5`, `@YES`, `@(expr)` compiles to).
Accessors: `intValue`, `integerValue`, `longValue`, `longLongValue`, `unsignedIntegerValue`, `boolValue`, `doubleValue`, `floatValue`, `stringValue`; `isEqualToNumber:`,
`isEqual:`, `hash` (equal across integer and floating storage), `compare:`, `description`, `copyWithZone:`.
Values are stored as BOOL, signed 64-bit or double; unsigned 64-bit values above `INT64_MAX` keep their bit pattern as a signed value. No `NSDecimalNumber`, no formatters.

## NSArray / NSMutableArray

`+array`, `+arrayWithObject:`, `+arrayWithObjects:count:` (array literals), `+arrayWithObjects:` (nil-terminated), `+arrayWithArray:`, `-initWithObjects:count:`,
`-initWithObjects:`, `-count`, `-objectAtIndex:`, `-objectAtIndexedSubscript:` (`a[i]`), `-firstObject`, `-lastObject`, `-indexOfObject:`, `-containsObject:`,
`-isEqualToArray:`, `-isEqual:`, `-hash`, `-description` (Apple's `(\n    a,\n    b\n)` layout), `-componentsJoinedByString:`, `-arrayByAddingObject:`,
`-countByEnumeratingWithState:objects:count:` (`for…in`), `-copyWithZone:`, `-mutableCopyWithZone:`.
Mutable: `+arrayWithCapacity:`, `-initWithCapacity:`, `-addObject:`, `-insertObject:atIndex:`, `-removeObjectAtIndex:`, `-removeLastObject`, `-removeAllObjects`,
`-removeObject:`, `-replaceObjectAtIndex:withObject:`, `-setObject:atIndexedSubscript:` (index == count appends).
Elements are compared with the guest's `-isEqual:`. Not implemented: sorting, blocks, predicates, subarrays, binary search, KVC, **mutation detection** during
fast enumeration (the mutation pointer always reads 0).

## NSDictionary / NSMutableDictionary

`+dictionary`, `+dictionaryWithObject:forKey:`, `+dictionaryWithObjects:forKeys:count:` (dictionary literals), `+dictionaryWithDictionary:`, `-initWithObjects:forKeys:count:`,
`-count`, `-objectForKey:`, `-objectForKeyedSubscript:` (`d[k]`), `-allKeys`, `-allValues`, `-isEqualToDictionary:`, `-isEqual:`, `-hash`, `-description`
(`{\n    k = v;\n}`, insertion order — Apple's order is unspecified), `-countByEnumeratingWithState:objects:count:` (enumerates keys), `-copyWithZone:`, `-mutableCopyWithZone:`.
Mutable: `+dictionaryWithCapacity:`, `-initWithCapacity:`, `-setObject:forKey:`, `-setObject:forKeyedSubscript:` (nil removes), `-removeObjectForKey:`, `-removeAllObjects`.
Keys are copied (`-copy`) and compared with the guest's `-hash`/`-isEqual:`; lookup is a hash-filtered **linear scan, O(n)**.
Not implemented: key/value enumerators, key sets, sorting, `writeToFile:`, property lists.

## NSAutoreleasePool

`-init` (pushes a pool), `-drain`, `-release` (both pop it). `@autoreleasepool` compiles to `objc_autoreleasePoolPush/Pop` and works with or without ARC.

## Compiling a test program

There is no Apple SDK in the repository. `tests/fixtures/objc/include/Foundation/Foundation.h` is a hand-written header declaring exactly the subset above and
`tests/fixtures/objc/sysroot/System/Library/Frameworks/Foundation.framework/Foundation.tbd` a text stub for ld64.lld. Programs are built by
`tools\llvm-env.cmd tools\gen-objc-fixtures.cmd` (`-fobjc-arc -fno-objc-exceptions` for ARC). Fixtures:

| Program | What it proves |
|---|---|
| `foundation_hello` | The reference program: `@"CIDER"`, `@[@20, @22]`, `for…in`, `NSLog(@"%@ result: %ld")` → `CIDER result: 42`, exit code 42, 0 live objects. |
| `foundation_string` | NSString behaviour including UTF-16 literals, formatting and ranges (exit code = failed checks). |
| `foundation_collections` | NSNumber, NSArray, NSMutableArray (100-element fast enumeration), NSDictionary, NSMutableDictionary, copies, subscripts, descriptions. |
| `foundation_subclass` | Guest subclasses of NSObject (MRC): retain counts, dealloc balance, `-description` used by `%@`, `-hash`/`-isEqual:` keys, copied keys, explicit `NSAutoreleasePool`. |
| `foundation_error` | Ten misuse modes that must end the guest with an exception-style message. |

Run one: `cider run tests\fixtures\objc\bin\foundation_hello\foundation_hello` (stdout: log lines and `Guest exit code: 42`; stderr: `CIDER result: 42`).

## 0.5 additions

New classes (all host-written, all exercised by compiled fixtures): **NSMutableString** (`appendString:`, `appendFormat:`, `setString:`, `insertString:atIndex:`, `deleteCharactersInRange:`,
`replaceCharactersInRange:withString:`, `stringWithCapacity:`; `copy` of a mutable string is an immutable snapshot), **NSData / NSMutableData** (`dataWithBytes:length:`, `length`, `bytes`, `getBytes:length:`, `isEqualToData:`,
`subdataWithRange:`, `description` as `<0102 0304>`, `appendBytes:length:`, `appendData:`, `setLength:`, `replaceBytesInRange:withBytes:`, `mutableBytes`), **NSDate** (`date`, `dateWithTimeInterval...`, `timeIntervalSince1970`,
`timeIntervalSinceNow`, `dateByAddingTimeInterval:`, `compare:`, `description` always in UTC), **NSURL** (`URLWithString:`, `fileURLWithPath:`, `absoluteString`, `path`, `scheme`, `host`, `query`, `lastPathComponent`,
`pathExtension`, `URLByAppendingPathComponent:`), **NSError** (`errorWithDomain:code:userInfo:`, `domain`, `code`, `userInfo`, `localizedDescription`, constants `NSLocalizedDescriptionKey`, `NSFilePathErrorKey`, `NSCocoaErrorDomain`...),
**NSFileManager** (`defaultManager`, `fileExistsAtPath:`(`isDirectory:`), `contentsAtPath:`, `createFileAtPath:contents:attributes:`, `removeItemAtPath:error:`, `createDirectoryAtPath:withIntermediateDirectories:attributes:error:`,
`contentsOfDirectoryAtPath:error:`, `currentDirectoryPath` = "/"), **NSNotification / NSNotificationCenter**, **NSMethodSignature / NSInvocation**, **NSException** (uncaught only), NSString path helpers (`lastPathComponent`,
`pathExtension`, `stringByDeletingLastPathComponent`, `stringByAppendingPathComponent:`, `isAbsolutePath`), block enumeration (`enumerateObjectsUsingBlock:`, `indexOfObjectPassingTest:`, `enumerateKeysAndObjectsUsingBlock:`).

**File access respects `--root`**: a guest path is resolved lexically against the guest root, re-checked after link resolution and refused outside it; without `--root` every file operation fails (`createFile` returns NO, `contentsAtPath`
returns nil, errors report "no permission"), exactly like `fopen`. The host filesystem outside the root is never reachable, and `removeItemAtPath:@"/"` refuses to delete the root itself. Not a sandbox: the guest still runs with the
rights of the worker. Errors use `NSCocoaErrorDomain` codes 4 (no such file), 513 (no permission), 516 (exists), 256 (other).

## CoreFoundation (0.5.5)

The CF types CIDER knows **are** the Foundation objects (a `CFStringRef` is an `NSString`, a `CFArrayRef` an `NSArray`...): toll-free bridging by construction for exactly these types, no general bridging layer.
Exported (CoreFoundation group): `CFRetain/Release/GetRetainCount/GetTypeID/Equal/Hash/CopyDescription/Show`, `CFString*` (create from a C string in UTF-8/ASCII/Latin-1, `CreateWithFormat` with `%@`, length, character at index, `GetCString`,
`GetCStringPtr`, compare, copy, mutable create/append), `CFArray*` / `CFDictionary*` (create, mutable create, count, get, append/set/add/remove, keys and values; only the `kCFType...CallBacks` are accepted: NULL or custom callbacks are refused),
`CFNumberCreate/GetValue`, `CFAbsoluteTimeGetCurrent`, and the run loop ([EVENT-LOOP.md](EVENT-LOOP.md)): `CFRunLoopGetMain/GetCurrent/Run/RunInMode/Stop/WakeUp`, version-0 sources, timers. Constants: `kCFAllocatorDefault`, `kCFTypeArrayCallBacks`,
`kCFRunLoopDefaultMode`... **Not implemented**: CFData, CFDate, CFURL, CFSet, CFBoolean objects, CFAllocator (the argument is ignored), CFPropertyList, CFPreferences, observers, run loop modes.
