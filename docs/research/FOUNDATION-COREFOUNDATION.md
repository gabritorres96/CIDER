# Foundation and CoreFoundation

Question: what do real programs need from Foundation/CoreFoundation, what does ORVYN have, and can open implementations help?
Labels: **VERIFIED**, **INFERRED**, **HYPOTHESIS**, **PENDING VALIDATION**.

## 1. Sources

| Source | Version | License | Platform |
|---|---|---|---|
| Apple `CF` | `CF-1153.18` `dc54c6b`, last pushed 2021-10-06; **not part of the macOS 26.0 `release.json`** | APSL 2.0 | Darwin only; stale |
| `swift-corelibs-foundation` → `Sources/CoreFoundation` (96 files, 11 MB) | `main` `384ff68` (2026-10-09) | Apache 2.0; each C file: "Licensed under Apache License v2.0 with Runtime Library Exception" (e.g. `CFString.c` header) | **Windows supported** (`TARGET_OS_WIN32` branches: `CFString.c` 8, `CFRunLoop.c` 27, `CFPlatform.c` 34, `CFLocale.c` 37, `CFRuntime.c` 15) |
| `swift-corelibs-foundation` → `Sources/Foundation` (Swift) | same | Apache 2.0 | the *Swift* Foundation for non-Darwin; not what Objective-C binaries call |
| ORVYN | `src/foundation/` (3,353 lines), `src/corefoundation/` (1,301), `docs/FOUNDATION.md` | own | |

Apple's Foundation (Objective-C classes and the Swift overlay) and AppKit are **not open source**: no repository for them exists in `distribution-macOS` (VERIFIED by project name in `release.json`).

## 2. What ORVYN has (VERIFIED, registry + classes)

* **Classes (host-written, payload objects):** `NSObject`, `NSString`/`NSConstantString`/`NSMutableString`, `NSNumber`, `NSArray`/`NSMutableArray`, `NSDictionary`/`NSMutableDictionary`, `NSData`/`NSMutableData`, `NSDate`, `NSURL`, `NSError`, `NSFileManager` (confined to `--root`), `NSNotification`/`NSNotificationCenter`, `NSMethodSignature`, `NSInvocation`, `NSException`, `NSAutoreleasePool`.
* **CoreFoundation (62 functions):** retain/release/type ids/equal/hash/description, `CFString` (create from C string/format, length, characters, compare, copy, mutable, append), `CFArray`, `CFDictionary`, `CFNumber`/`CFBoolean`, `CFAbsoluteTimeGetCurrent`, `CFRunLoop` (one mode: main, current, run, stop, sources, timers). **Design:** CF types *are* the Foundation objects ("toll-free bridged by construction"), so `objc_msgSend` on a `CFStringRef` works.
* **Not present:** `NSSet`/`NSMutableSet`/`NSOrderedSet`, `NSNull`, `NSValue`, `NSIndexSet`, `NSCharacterSet`, `NSScanner`, `NSDateFormatter`/`NSNumberFormatter`, `NSLocale`/`NSTimeZone`/`NSCalendar`, `NSBundle`, `NSUserDefaults`, `NSRunLoop`/`NSTimer` (0.6.7 plan), `NSThread`/`NSOperationQueue`, `NSProcessInfo`, `NSUUID`, `NSPredicate`, `NSJSONSerialization`, `NSPropertyListSerialization`, `NSCoder`/`NSKeyedArchiver`, `NSURLSession`, `NSTask`, `NSAttributedString`, `NSUndoManager`, KVC/KVO, `NSCopying`/`NSCoding` protocols as behavior; CF: `CFData`, `CFDate`, `CFURL`, `CFSet`, `CFError`, `CFBundle`, `CFPreferences`, `CFNotificationCenter`, `CFAllocator`, `CFPropertyList`, `CFCharacterSet`, `CFLocale`, `CFTimeZone`, `CFCalendar`, `CFUUID`, `CFStream`.

## 3. What real programs ask for (VERIFIED, CotEditor 7.0.9 import list)

| Library | Imported symbols | Swift-mangled (`_$s...`) | ObjC class refs | other |
|---|---|---|---|---|
| Foundation | 749 | **698 (93 %)** | 34 | 17 (`_NSFileSize`, `_NSHomeDirectory`, `_NSLocalizedDescriptionKey`, `_NSKeyValueChangeNewKey`...) |
| AppKit | 310 | 112 | 134 | 64 |
| CoreFoundation | 33 | 0 | 10 | 23 (`_CFStringConvert*`, `_kCFBundleNameKey`, `_kCFAllocatorDefault`) |
| libSystem | 72 | 0 | 1 | 71 |
| libswiftCore | 1,316 | 1,186 | 2 | 128 |
| SwiftUI | 1,267 | 1,267 | 0 | 0 |

Consequence (INFERRED from the numbers): for a **Swift** application the Foundation dependency is almost entirely the *Swift overlay* (`$s10Foundation...`: `CocoaError`, `Data`, `Date`, `URL`, `Codable` support...), which is **compiled Swift code inside Apple's Foundation binary**. An Objective-C-only provider like ORVYN's cannot satisfy it; see [SWIFT-RUNTIME-FEASIBILITY.md](SWIFT-RUNTIME-FEASIBILITY.md). For an **Objective-C** application the demand is classes and string/URL/file constants — the part ORVYN is growing.

## 4. Semantics that matter (and where ORVYN differs)

| Topic | Real behavior | ORVYN | Note |
|---|---|---|---|
| Memory | ARC + autorelease pools; `CFRetain`/`CFRelease`/`CFAutorelease`; "Create" and "Copy" rule (+1), "Get" rule (+0) | objects in `live_`; pools; CF functions follow the same rules (VERIFIED in `cf_bridge.cpp`: `CFStringCreateWithCString` returns +1) | `CFAutorelease` missing |
| Toll-free bridging | `CFStringRef` ↔ `NSString` share `isa` (`__NSCFString`, `NSTaggedPointerString`) | same by construction; no tagged pointers | OK until a guest reads tagged-pointer bits |
| Strings | NSString is UTF-16 semantically (`length`, `characterAtIndex:` in UTF-16 units) | UTF-8 storage + UTF-16 views (`utf16_length`, `to_u16`) | O(n) indexing; `rangeOfString:`, case mapping, normalisation, `localizedCompare:` need ICU or own tables |
| Collections | `CFBasicHash` (open addressing) | `NSDictionary`: hash-filtered **linear scan** (documented O(n)) | OK for fixtures, slow for real apps |
| Run loop | `CFRunLoop` modes, observers, `CFRunLoopSource` v0/v1, timers, `kCFRunLoopCommonModes`; on Windows the swift-corelibs build waits with `MsgWaitForMultipleObjectsEx` (`CFRunLoop.c:3021`) | one mode, sources + timers, wakes on a Win32 event + input queue (`wait()` in the GUI backend) | the same Win32 primitive; modes and observers missing |
| Preferences | `CFPreferences` talks to `cfprefsd` over XPC; `NSUserDefaults` caches | absent; 0.6.8 plan: atomic JSON store under a per-application data directory | the corelibs `CFPreferences.c` (792 lines, 3 WIN32 refs) is a file-backed model that can serve as semantic reference |
| Bundles | `CFBundle`/`NSBundle`: `Info.plist`, `Resources`, localizations, `mainBundle` | `src/bundle` parses `.app` for inspection only | `NSBundle mainBundle` needed by almost every app |
| Files | `NSFileManager` / `NSURL` file URLs, attributes, `enumeratorAtURL`, security-scoped bookmarks | root-confined subset | path semantics decision (guest sees a root; see LIBSYSTEM doc) |
| Locale / time zones / calendars | ICU-backed | absent | needs ICU: system `icu.dll` or bundled |

## 5. Can open implementations be reused? (analysis)

`swift-corelibs-foundation/Sources/CoreFoundation` is the **only permissively licensed, buildable-on-Windows CoreFoundation**. But it is *a different object model*: on non-Darwin it has no Objective-C bridge; `CFRuntimeBase` objects have their own `_cfisa`; CF types are not NS objects. ORVYN's guests call `objc_msgSend` on CF objects and hold `CFSTR("...")` constants (`__cfstring` section with Apple's `__CFConstStr` layout), so adopting the corelibs CF *as the object model* would mean building the NS↔CF bridge ORVYN deliberately avoided, on a runtime whose class layout differs from Apple's (VERIFIED difference in the OBJC document for libobjc2; the same reasoning applies to corelibs' own bridge).

**Therefore (INFERRED):** keep ORVYN's object model; **adapt algorithms and tables** from the corelibs CF where ORVYN needs real behavior and the file is self-contained:

| Need | Candidate file(s) in swift-corelibs `Sources/CoreFoundation` | Notes |
|---|---|---|
| Hash tables with CF semantics (fix the linear-scan dictionary) | `CFBasicHash.c` (1,774 lines), `CFBasicHashFindBucket.inc` | pure C, no Darwin dependencies visible in the includes; strategy B (adapt) or C (reimplement with the same semantics) |
| String encodings, Unicode decomposition/precomposition | `CFStringEncodingConverter*.c`, `CFUnicodeDecomposition.c`, `CFUnicodePrecomposition.c`, `CFICUConverters.c` | tables + code; ICU variant needs ICU |
| Date/time math | `CFDate.c` (588), `CFCalendar.c` (3,012; ICU) | ICU for calendars; Gregorian math is portable |
| URL parsing/escaping | `CFURL.c` (5,494) | complex but self-contained; ORVYN's `NSURL` is a minimal parser today |
| Number formatting | `CFNumberFormatter.c` (ICU) | needs ICU |
| Property lists | `CFPropertyList.c` (3,288) | ORVYN already has an XML+binary reader (`src/corefoundation/plist.cpp`); use as a *test oracle* |
| Run loop semantics (modes/observers) | `CFRunLoop.c` (4,892, Windows branch) | reference for how to wait for Win32 messages, handles and timers together |
| Preferences model | `CFPreferences.c` | reference |

Licensing is straightforward for these files (Apache 2.0 with Runtime Library Exception: keep notices, mark modifications; see [THIRD-PARTY-LICENSE-REVIEW.md](THIRD-PARTY-LICENSE-REVIEW.md)). Compare Apple's `CF-1153.18` (APSL 2.0): **prefer the Swift fork** because its license is simpler and it builds for Windows.

`swift-corelibs-foundation/Sources/Foundation` (Swift) and `FoundationNetworking`/`FoundationXML` are **not** usable for Objective-C guests (they are Swift code) and are only relevant to the Swift feasibility analysis (and still would not match Apple's overlay layouts, see the Swift document).

## 6. Recommendations

1. Replace the linear-scan `NSDictionary`/`NSSet` backing with a hash table using CF semantics (adapt `CFBasicHash`, or a tested reimplementation): prerequisite for real apps.
2. Add the missing *core* classes in this order: `NSSet`/`NSMutableSet`, `NSNull`, `NSValue`, `NSIndexSet`, `NSCharacterSet`, `NSBundle` (+`CFBundle` basics), `NSUserDefaults`, `NSTimer`/`NSRunLoop`, `NSProcessInfo`, `NSUUID`, `NSLocale`/`NSTimeZone`.
3. Decide the **ICU strategy** once (system ICU vs bundled) before `NSString` text operations, `NSDateFormatter`, `NSNumberFormatter`, `NSCalendar`; PENDING VALIDATION of Windows' ICU availability on the supported versions.
4. Keep an *oracle-driven* approach: each class gets a fixture compiled by clang and checked against documented Apple behavior; the corelibs implementation can serve as a second opinion for edge cases, not as the specification (it is **not** Apple's Foundation).
5. KVC/KVO: needs associated objects and runtime dynamic subclassing ([OBJC document](OBJC-RUNTIME-COMPATIBILITY.md)).
