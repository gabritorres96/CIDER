# CotEditor 7.0.9 compatibility analysis (CIDER 0.5)

Reference application: `CotEditor_7.0.9.dmg` (from `utilities/`, git-ignored), a real, signed macOS text editor. **CotEditor does not run in CIDER and is not expected to in 0.5.**
CIDER refuses it before any guest instruction executes: `cider run CotEditor.app` exits with code 12 and lists the missing system libraries (dozens).
Everything below is static analysis (`cider inspect`, `cider dependencies`, `llvm-objdump --chained-fixups`, `tools/app_coverage.py`); nothing was executed.

## Answers

**1. Architecture.** Universal binary (FAT): x86_64 + arm64. The x86_64 slice is a 52.5 MB `MH_EXECUTE`, platform macOS, minimum **15.0**, built with SDK 26.5, uses chained fixups.
The bundle's `LSMinimumSystemVersion` is 15.0. CIDER executes only x86_64 (arm64 is parsed and ignored), so the slice is the right one; its age is not the obstacle.

**2. Main dependencies.** About 55 linked libraries (transitive list in `cider dependencies CotEditor.app`): Foundation, AppKit, CoreGraphics, CoreFoundation, libobjc, libSystem, SwiftUI,
Combine, AppIntents, StoreKit, VisionKit, AVFoundation, CoreText, ImageIO, QuartzCore, QuickLookUI, OSAKit, IOKit, Symbols, DeveloperToolsSupport, CoreTransferable, libc++, and **26 Swift
runtime/overlay libraries** (`libswiftCore`, `libswift_Concurrency`, `libswift_StringProcessing`, `libswiftObservation`, `libswiftSynchronization`, `libswiftDispatch`...).
It bundles one framework, `Sparkle.framework` (updater), which in turn needs libcompression, Security, ServiceManagement, SystemConfiguration, WebKit and IOKit.

**3. Objective-C, Swift or both.** Both, mostly **Swift**: the executable has `__swift5_*` sections (types, protocols, fields, reflection strings, capture descriptors, entry point),
a SwiftUI dependency, and 154 imported Objective-C classes. Its code is Swift with AppKit/Foundation bridges.

**4. APIs that are missing in CIDER** (measured on the 4,371 symbols the x86_64 slice imports; "implemented" = a host function/class exists, not verified behaviour):

| Imported from | Imported symbols | CIDER implements |
|---|---:|---:|
| libswiftCore | 1,316 | 0 |
| SwiftUI | 1,267 | 0 |
| Foundation | 749 | 6 |
| AppKit | 310 | 7 |
| libswift_Concurrency | 96 | 0 |
| Combine | 88 | 0 |
| libSystem | 72 | 37 |
| AppIntents | 62 | 0 |
| CoreGraphics | 53 | 16 |
| libswiftUniformTypeIdentifiers | 45 | 0 |
| StoreKit | 39 | 0 |
| CoreFoundation | 33 | 6 |
| libobjc | 19 | 14 |
| everything else (Swift overlays, VisionKit, CoreTransferable, Symbols, CoreText, IOKit, ...) | 222 | 0 |
| **Total** | **4,371** | **86 (2.0 %)** |

Not implemented libobjc imports: `_objc_msgSendSuper2_stret`, `_objc_opt_self`, `_objc_setHook_getClass`, `__objc_empty_cache` as an import, `_OBJC_METACLASS_$_NSObject`.
The detailed per-symbol lists are in `utilities/reports/coverage_CotEditor.json` (not versioned).

**5. What blocks execution today** (in the order `cider run` hits them):
1. Dozens of system libraries do not exist in CIDER (reported by name; CIDER never loads macOS system libraries nor substitutes Windows DLLs).
2. **The Swift runtime**: `libswiftCore` and 25 overlays (metadata, protocol witness tables, reference counting, generics, async/await runtime, `String`/`Array` implementations in Swift). Nothing of this exists and
   it cannot be approximated host-side: Swift code calls them directly (1,316 imports from libswiftCore alone).
3. SwiftUI (1,267 imports): a declarative UI framework layered over AppKit/CoreGraphics/QuartzCore/Metal; far larger than the AppKit subset.
4. Features CIDER lacks even before that: Swift-style exceptions/unwinding, `libc++`, threads beyond the cooperative model, text system (NSTextView, CoreText), menus, documents (`NSDocument`), file dialogs, XPC.

**6. What to implement first** (value per effort, for *any* real application, not only CotEditor):
1. The Objective-C surface that Objective-C-only apps use: `NSMenu`/`NSMenuItem`, `NSTextView` basics, `NSImage`, `NSTimer`, `NSUserDefaults`, `NSBundle`, key-value coding, properties/KVO.
2. Drawing: CoreGraphics contexts (`CGContext`, paths, colours) and `drawRect:` for custom views.
3. `libc++`/`libc++abi` import surface and C++ exception handling.
4. A Swift strategy is a separate research project (evaluate shipping a Windows build of the open-source Swift runtime against Apple's metadata ABI). Until then, applications built with Swift are out of reach.
5. Only then SwiftUI.

## Status

* CotEditor is **not** a success criterion of CIDER 0.5.0 and **no claim** is made that it works.
* The same analysis for Rectangle, Stats and MonitorControl is in [COMPATIBILITY.md](COMPATIBILITY.md): all three are Swift applications and are blocked by the same wall.
