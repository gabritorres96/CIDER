/* Hand-written declarations of the AppKit subset CIDER implements (see docs/APPKIT.md). Not Apple's AppKit.h: the Apple SDK is not redistributed. */
#ifndef CIDER_APPKIT_H
#define CIDER_APPKIT_H
#include "../Foundation/Foundation.h"

enum { NSWindowStyleMaskBorderless = 0, NSWindowStyleMaskTitled = 1, NSWindowStyleMaskClosable = 2, NSWindowStyleMaskMiniaturizable = 4, NSWindowStyleMaskResizable = 8 };
enum { NSBackingStoreBuffered = 2 };
enum { NSApplicationActivationPolicyRegular = 0, NSApplicationActivationPolicyAccessory = 1, NSApplicationActivationPolicyProhibited = 2 };
enum { NSViewMinXMargin = 1, NSViewWidthSizable = 2, NSViewMaxXMargin = 4, NSViewMinYMargin = 8, NSViewHeightSizable = 16, NSViewMaxYMargin = 32 };
enum { NSButtonTypeSwitch = 3, NSButtonTypeMomentaryPushIn = 7 };
enum { NSBezelStyleRounded = 1 };
enum { NSControlStateValueOff = 0, NSControlStateValueOn = 1 };
enum { NSEventTypeLeftMouseDown = 1, NSEventTypeLeftMouseUp = 2, NSEventTypeRightMouseDown = 3, NSEventTypeRightMouseUp = 4, NSEventTypeMouseMoved = 5,
       NSEventTypeLeftMouseDragged = 6, NSEventTypeKeyDown = 10, NSEventTypeKeyUp = 11 };
enum { NSTerminateCancel = 0, NSTerminateNow = 1, NSTerminateLater = 2 };

extern NSString *const NSApplicationDidFinishLaunchingNotification;
extern NSString *const NSApplicationWillTerminateNotification;
extern NSString *const NSWindowWillCloseNotification;
extern NSString *const NSWindowDidResizeNotification;

@class NSWindow, NSView, NSEvent, NSApplication;

@protocol NSApplicationDelegate
@optional
- (void)applicationDidFinishLaunching:(NSNotification *)notification;
- (NSInteger)applicationShouldTerminate:(NSApplication *)sender;
- (void)applicationWillTerminate:(NSNotification *)notification;
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender;
@end
@protocol NSWindowDelegate
@optional
- (BOOL)windowShouldClose:(NSWindow *)sender;
- (void)windowWillClose:(NSNotification *)notification;
- (void)windowDidResize:(NSNotification *)notification;
- (void)windowDidMove:(NSNotification *)notification;
- (void)windowDidBecomeKey:(NSNotification *)notification;
- (void)windowDidResignKey:(NSNotification *)notification;
@end

@interface NSResponder : NSObject
- (NSResponder *)nextResponder;
- (BOOL)acceptsFirstResponder;
- (BOOL)becomeFirstResponder;
- (BOOL)resignFirstResponder;
- (void)mouseDown:(NSEvent *)event;
- (void)mouseUp:(NSEvent *)event;
- (void)mouseDragged:(NSEvent *)event;
- (void)mouseMoved:(NSEvent *)event;
- (void)rightMouseDown:(NSEvent *)event;
- (void)rightMouseUp:(NSEvent *)event;
- (void)keyDown:(NSEvent *)event;
- (void)keyUp:(NSEvent *)event;
@end

@interface NSEvent : NSObject
- (NSUInteger)type;
- (NSPoint)locationInWindow;
- (NSWindow *)window;
- (NSString *)characters;
- (NSString *)charactersIgnoringModifiers;
- (unsigned short)keyCode;
- (NSUInteger)modifierFlags;
- (NSInteger)clickCount;
- (NSInteger)buttonNumber;
@end

@interface NSScreen : NSObject
+ (NSScreen *)mainScreen;
- (NSRect)frame;
- (NSRect)visibleFrame;
- (CGFloat)backingScaleFactor;
@end

@interface NSApplication : NSResponder
+ (NSApplication *)sharedApplication;
- (void)run;
- (void)stop:(id)sender;
- (void)terminate:(id)sender;
- (void)finishLaunching;
- (BOOL)setActivationPolicy:(NSInteger)policy;
- (NSInteger)activationPolicy;
- (void)activateIgnoringOtherApps:(BOOL)flag;
- (id<NSApplicationDelegate>)delegate;
- (void)setDelegate:(id<NSApplicationDelegate>)delegate;
- (NSArray *)windows;
- (NSWindow *)keyWindow;
- (NSWindow *)mainWindow;
- (BOOL)isRunning;
- (BOOL)isActive;
- (BOOL)sendAction:(SEL)action to:(id)target from:(id)sender;
@end
extern NSApplication *NSApp;

@interface NSView : NSResponder
- (instancetype)initWithFrame:(NSRect)frame;
- (NSRect)frame;
- (NSRect)bounds;
- (void)setFrame:(NSRect)frame;
- (void)setFrameSize:(NSSize)size;
- (void)setFrameOrigin:(NSPoint)origin;
- (void)addSubview:(NSView *)view;
- (void)removeFromSuperview;
- (NSView *)superview;
- (NSWindow *)window;
- (NSArray *)subviews;
- (void)setNeedsDisplay:(BOOL)flag;
- (BOOL)needsDisplay;
- (void)display;
- (void)displayIfNeeded;
- (void)setNeedsDisplayInRect:(NSRect)invalidRect;
- (BOOL)isHidden;
- (void)setHidden:(BOOL)hidden;
- (NSInteger)tag;
- (void)setTag:(NSInteger)tag;
- (NSView *)viewWithTag:(NSInteger)tag;
- (NSUInteger)autoresizingMask;
- (void)setAutoresizingMask:(NSUInteger)mask;
- (BOOL)isFlipped;
- (BOOL)isDescendantOf:(NSView *)view;
- (void)drawRect:(NSRect)dirtyRect;
@end

@interface NSGraphicsContext : NSObject
+ (NSGraphicsContext *)currentContext;
+ (void)setCurrentContext:(NSGraphicsContext *)context;
+ (NSGraphicsContext *)graphicsContextWithCGContext:(CGContextRef)graphicsPort flipped:(BOOL)initialFlippedState;
+ (void)saveGraphicsState;
+ (void)restoreGraphicsState;
- (CGContextRef)CGContext;
- (void *)graphicsPort;
- (BOOL)isFlipped;
- (void)flushGraphics;
- (void)saveGraphicsState;
- (void)restoreGraphicsState;
@end

@interface NSControl : NSView
- (id)target;
- (void)setTarget:(id)target;
- (SEL)action;
- (void)setAction:(SEL)action;
- (BOOL)isEnabled;
- (void)setEnabled:(BOOL)enabled;
- (NSString *)stringValue;
- (void)setStringValue:(NSString *)string;
- (int)intValue;
- (NSInteger)integerValue;
- (void)setIntValue:(int)value;
- (void)setIntegerValue:(NSInteger)value;
- (double)doubleValue;
- (void)setDoubleValue:(double)value;
- (BOOL)sendAction:(SEL)action to:(id)target;
- (void)performClick:(id)sender;
- (void)sizeToFit;
@end

@interface NSButton : NSControl
+ (instancetype)buttonWithTitle:(NSString *)title target:(id)target action:(SEL)action;
- (NSString *)title;
- (void)setTitle:(NSString *)title;
- (NSInteger)state;
- (void)setState:(NSInteger)state;
- (void)setButtonType:(NSUInteger)type;
- (void)setBezelStyle:(NSUInteger)style;
@end

@interface NSTextField : NSControl
+ (instancetype)labelWithString:(NSString *)string;
+ (instancetype)wrappingLabelWithString:(NSString *)string;
- (BOOL)isEditable;
- (void)setEditable:(BOOL)editable;
- (BOOL)isSelectable;
- (void)setSelectable:(BOOL)selectable;
- (BOOL)isBezeled;
- (void)setBezeled:(BOOL)bezeled;
- (void)setDrawsBackground:(BOOL)flag;
- (NSString *)placeholderString;
- (void)setPlaceholderString:(NSString *)string;
- (id)delegate;
- (void)setDelegate:(id)delegate;
@end

@interface NSWindow : NSResponder
- (instancetype)initWithContentRect:(NSRect)contentRect styleMask:(NSUInteger)style backing:(NSUInteger)backing defer:(BOOL)flag;
- (NSString *)title;
- (void)setTitle:(NSString *)title;
- (NSView *)contentView;
- (void)setContentView:(NSView *)view;
- (NSRect)frame;
- (void)setFrame:(NSRect)frame display:(BOOL)display;
- (void)setContentSize:(NSSize)size;
- (void)setFrameOrigin:(NSPoint)origin;
- (void)center;
- (void)setMinSize:(NSSize)size;
- (NSSize)minSize;
- (NSUInteger)styleMask;
- (NSInteger)level;
- (void)setLevel:(NSInteger)level;
- (id<NSWindowDelegate>)delegate;
- (void)setDelegate:(id<NSWindowDelegate>)delegate;
- (BOOL)isReleasedWhenClosed;
- (void)setReleasedWhenClosed:(BOOL)flag;
- (void)makeKeyAndOrderFront:(id)sender;
- (void)orderFront:(id)sender;
- (void)orderOut:(id)sender;
- (void)close;
- (void)performClose:(id)sender;
- (BOOL)isVisible;
- (BOOL)isKeyWindow;
- (BOOL)isMainWindow;
- (void)makeKeyWindow;
- (BOOL)makeFirstResponder:(NSResponder *)responder;
- (NSResponder *)firstResponder;
- (void)display;
@end

#endif
