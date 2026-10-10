/* AppKit subset behaviour from guest code (ARC). Designed for the offscreen backend, whose scripted input is (see test_appkit.cpp):
       wait 10 / click Add / click Add / click Toggle / click Disabled / type abc / mouse 60 240 / key z / close / close
   Everything is verified inside callbacks; the program stops the run loop after the second close and exits with the number of failed checks. */
#include "AppKit/AppKit.h"

static int fails;
#define CHECK(...) do { if (!(__VA_ARGS__)) { fails++; NSLog(@"FAIL line %d: %s", __LINE__, #__VA_ARGS__); } } while (0)

static int adds, toggles, disabled_hits, close_attempts, will_close_calls, notification_calls, mouse_downs, key_downs, resizes;
static NSPoint mouse_at;
static NSString *last_keys;

@interface Canvas : NSView
@end
@implementation Canvas
- (void)mouseDown:(NSEvent *)event {
    mouse_downs++;
    mouse_at = [event locationInWindow];
    CHECK([event type] == NSEventTypeLeftMouseDown && [event window] != nil && [event clickCount] == 1);
}
- (BOOL)acceptsFirstResponder { return YES; }
- (void)keyDown:(NSEvent *)event {
    key_downs++;
    last_keys = [event characters];
}
@end

@interface Controller : NSObject <NSApplicationDelegate, NSWindowDelegate> {
@public
    NSWindow *window;
    NSButton *add, *toggle, *disabled;
    NSTextField *field, *status;
    Canvas *canvas;
}
- (void)add:(id)sender;
- (void)toggle:(id)sender;
- (void)never:(id)sender;
- (void)verify;
@end

@implementation Controller
- (void)add:(id)sender { adds++; CHECK(sender == add); [status setStringValue:[NSString stringWithFormat:@"adds=%d", adds]]; }
- (void)toggle:(id)sender { toggles++; CHECK([toggle state] == NSControlStateValueOn); }
- (void)never:(id)sender { disabled_hits++; }
- (BOOL)windowShouldClose:(NSWindow *)sender {
    close_attempts++;
    return close_attempts >= 2;                      /* the first close request is refused */
}
- (void)windowWillClose:(NSNotification *)note {
    will_close_calls++;
    CHECK([[note name] isEqualToString:NSWindowWillCloseNotification] && [note object] == window);
    [self verify];
    [NSApp stop:nil];
}
- (void)windowDidResize:(NSNotification *)note { resizes++; }
- (void)windowClosed:(NSNotification *)note { notification_calls++; }
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender { return NO; }

- (void)verify {
    CHECK(adds == 2 && toggles == 1 && disabled_hits == 0);
    CHECK(close_attempts == 2 && will_close_calls == 1);
    CHECK([[field stringValue] isEqualToString:@"abc"]);
    CHECK([[status stringValue] isEqualToString:@"adds=2"]);
    CHECK(mouse_downs == 1 && mouse_at.x == 60 && mouse_at.y == 60);      /* window space, y up: 300 - 240 */
    CHECK(key_downs == 1 && [last_keys isEqualToString:@"z"]);
    CHECK(resizes >= 1);
}
@end

int main(void) {
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        CHECK(app != nil && app == NSApp && app == [NSApplication sharedApplication]);
        CHECK([app setActivationPolicy:NSApplicationActivationPolicyRegular] && [app activationPolicy] == NSApplicationActivationPolicyRegular);
        CHECK(![app isRunning] && [[app windows] count] == 0);

        Controller *c = [[Controller alloc] init];
        [app setDelegate:c];
        CHECK([app delegate] == c);

        NSRect frame = NSMakeRect(100, 100, 400, 300);
        c->window = [[NSWindow alloc] initWithContentRect:frame
                                                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];
        NSWindow *w = c->window;
        CHECK([[app windows] count] == 1 && [[app windows] objectAtIndex:0] == w);
        CHECK(![w isVisible] && [[w title] length] == 0);
        [w setTitle:@"Basic"];
        [w setDelegate:c];
        [w setMinSize:NSMakeSize(200, 150)];
        CHECK([w minSize].width == 200 && [w minSize].height == 150);
        CHECK([w styleMask] == (NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable));
        CHECK([w contentView] != nil && [[w contentView] window] == w && [[w contentView] superview] == nil);
        CHECK([[w contentView] frame].size.width == 400 && [[w contentView] frame].size.height == 300);
        CHECK(NSEqualRects([[w contentView] bounds], NSMakeRect(0, 0, 400, 300)));
        [[NSNotificationCenter defaultCenter] addObserver:c selector:@selector(windowClosed:) name:NSWindowWillCloseNotification object:w];

        NSView *content = [w contentView];
        c->add = [[NSButton alloc] initWithFrame:NSMakeRect(10, 260, 80, 28)];
        [c->add setTitle:@"Add"]; [c->add setTarget:c]; [c->add setAction:@selector(add:)]; [c->add setTag:1];
        c->toggle = [[NSButton alloc] initWithFrame:NSMakeRect(100, 260, 80, 28)];
        [c->toggle setTitle:@"Toggle"]; [c->toggle setButtonType:NSButtonTypeSwitch]; [c->toggle setTarget:c]; [c->toggle setAction:@selector(toggle:)];
        c->disabled = [[NSButton alloc] initWithFrame:NSMakeRect(190, 260, 80, 28)];
        [c->disabled setTitle:@"Disabled"]; [c->disabled setTarget:c]; [c->disabled setAction:@selector(never:)]; [c->disabled setEnabled:NO];
        c->field = [[NSTextField alloc] initWithFrame:NSMakeRect(10, 220, 200, 24)];
        [c->field setEditable:YES]; [c->field setPlaceholderString:@"type here"];
        c->status = [NSTextField labelWithString:@"adds=0"];
        [c->status setFrame:NSMakeRect(10, 190, 200, 20)];
        c->canvas = [[Canvas alloc] initWithFrame:NSMakeRect(10, 10, 200, 100)];
        [c->canvas setAutoresizingMask:NSViewWidthSizable];
        [c->canvas setTag:99];
        for (NSView *v in @[c->add, c->toggle, c->disabled, c->field, c->status, c->canvas]) [content addSubview:v];

        CHECK([[content subviews] count] == 6 && [c->add superview] == content && [c->add window] == w);
        CHECK([content viewWithTag:99] == c->canvas && [content viewWithTag:1] == c->add && [content viewWithTag:12345] == nil);
        CHECK([c->canvas isDescendantOf:content] && ![content isDescendantOf:c->canvas]);
        CHECK([c->add isEnabled] && ![c->disabled isEnabled] && [c->field isEditable] && ![c->status isEditable]);
        CHECK([[c->add title] isEqualToString:@"Add"] && [[c->status stringValue] isEqualToString:@"adds=0"]);
        CHECK([c->toggle state] == NSControlStateValueOff);
        CHECK(c->add != nil && [c->add target] == c && [c->add action] == @selector(add:));

        /* a view can be removed and re-added; removal keeps it alive until the autorelease pool drains */
        NSView *scratch = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 10, 10)];
        [content addSubview:scratch];
        CHECK([[content subviews] count] == 7 && [scratch window] == w);
        [scratch removeFromSuperview];
        CHECK([[content subviews] count] == 6 && [scratch superview] == nil && [scratch window] == nil);
        [scratch setHidden:YES];
        CHECK([scratch isHidden]);

        /* geometry: frames, hit order and a window resize that autoresizes the canvas */
        CHECK(NSEqualRects([c->canvas frame], NSMakeRect(10, 10, 200, 100)));
        [w makeKeyAndOrderFront:nil];
        CHECK([w isVisible] && [app keyWindow] == w);
        [w setContentSize:NSMakeSize(500, 400)];
        CHECK([[w contentView] frame].size.width == 500 && [[w contentView] frame].size.height == 400);
        CHECK([c->canvas frame].size.width == 300 && [c->canvas frame].size.height == 100);      /* width sizable only */
        CHECK([c->add frame].size.width == 80);
        [w setContentSize:NSMakeSize(400, 300)];
        CHECK([c->canvas frame].size.width == 200);
        NSScreen *screen = [NSScreen mainScreen];
        CHECK([screen frame].size.width > 0 && [screen frame].size.height > 0 && [screen backingScaleFactor] >= 1.0);
        CHECK([w makeFirstResponder:c->canvas] && [w firstResponder] == c->canvas);

        [app activateIgnoringOtherApps:YES];
        [app run];

        CHECK(![app isRunning]);
        CHECK(notification_calls == 1);                    /* the NSNotificationCenter observer saw NSWindowWillCloseNotification */
        CHECK(![w isVisible] && [[app windows] count] == 0);
        last_keys = nil;                                   /* strong globals are never released by the process */
        NSLog(@"appkit checks done: %d failures", fails);
    }
    return fails;
}
