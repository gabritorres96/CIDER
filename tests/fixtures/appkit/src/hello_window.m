/* The ORVYN 0.5 reference program: a real Windows window created by Objective-C code compiled for macOS.
   Title "ORVYN - Hello from macOS", two labels, a counter, a "Click me" button and a "Close" button. The counter lives in this program (the AppController ivar),
   not in the host. Pressing "Click me" runs -buttonClicked: (found in this Mach-O by the Objective-C runtime), pressing "Close" closes the window; the delegate then ends
   the application, the event loop returns and the process exits with status 0. */
#include "AppKit/AppKit.h"

@interface AppController : NSObject <NSApplicationDelegate, NSWindowDelegate> {
    NSWindow *window;
    NSTextField *counterLabel;
    int clicks;
}
- (void)buildWindow;
- (void)buttonClicked:(id)sender;
- (void)closeClicked:(id)sender;
@end

@implementation AppController

- (void)buildWindow {
    NSRect frame = NSMakeRect(0, 0, 420, 240);
    window = [[NSWindow alloc] initWithContentRect:frame
                                         styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
                                           backing:NSBackingStoreBuffered
                                             defer:NO];
    [window setTitle:@"ORVYN — Hello from macOS"];
    [window setDelegate:self];

    NSView *content = [window contentView];

    NSTextField *hello = [NSTextField labelWithString:@"Hello from macOS!"];
    [hello setFrame:NSMakeRect(20, 190, 380, 24)];
    [content addSubview:hello];

    NSTextField *sub = [NSTextField labelWithString:@"Running through ORVYN 0.5"];
    [sub setFrame:NSMakeRect(20, 160, 380, 20)];
    [content addSubview:sub];

    counterLabel = [NSTextField labelWithString:@"Clicks: 0"];
    [counterLabel setFrame:NSMakeRect(20, 120, 380, 20)];
    [content addSubview:counterLabel];

    NSButton *click = [[NSButton alloc] initWithFrame:NSMakeRect(20, 60, 120, 32)];
    [click setTitle:@"Click me"];
    [click setTarget:self];
    [click setAction:@selector(buttonClicked:)];
    [content addSubview:click];

    NSButton *close = [[NSButton alloc] initWithFrame:NSMakeRect(160, 60, 120, 32)];
    [close setTitle:@"Close"];
    [close setTarget:self];
    [close setAction:@selector(closeClicked:)];
    [content addSubview:close];

    [window center];
}

- (void)showWindow {
    [window makeKeyAndOrderFront:nil];
}

- (void)buttonClicked:(id)sender {
    clicks++;
    [counterLabel setStringValue:[NSString stringWithFormat:@"Clicks: %d", clicks]];
    NSLog(@"Button clicked! count=%d", clicks);
}

- (void)closeClicked:(id)sender {
    NSLog(@"Close pressed");
    [window close];
}

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    NSLog(@"Application launched");
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    return YES;
}

- (void)windowWillClose:(NSNotification *)notification {
    NSLog(@"Window closing after %d click(s)", clicks);
}

- (void)applicationWillTerminate:(NSNotification *)notification {
    NSLog(@"Application terminating");
}

@end

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        AppController *controller = [[AppController alloc] init];
        [app setDelegate:controller];
        [controller buildWindow];
        [controller showWindow];
        [app activateIgnoringOtherApps:YES];
        [app run];
    }
    return 0;
}
