/* ORVYN 0.6 drawing life cycle, from the guest side. Three scenarios in one process, no event loop (drawing is forced with -display):
     A  40 windows are created, drawn three times each and closed: no object, native window or surface may survive them.
     B  a view closes its own window from inside -drawRect: (the surface and the window disappear in the middle of a repaint).
     C  a view asks for a repaint from inside -drawRect: (must not loop), and a CGContext kept after -drawRect: returned is used (must be refused, not crash).
   Exit code = number of failed checks. */
#include "AppKit/AppKit.h"

static int failures;
#define CHECK(...) do { if (!(__VA_ARGS__)) { failures++; NSLog(@"FAIL line %d: %s", __LINE__, #__VA_ARGS__); } } while (0)

static int draws, mode, dA, dB, dC;                 /* mode: 0 plain, 1 close the window inside drawRect:, 2 setNeedsDisplay: inside drawRect: and keep the context */
static CGContextRef kept;

@interface Probe : NSView
@end
@implementation Probe
- (void)drawRect:(NSRect)dirtyRect {
    draws++;
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGContextSetRGBFillColor(ctx, 0.2, 0.4, 0.6, 1);
    CGContextFillRect(ctx, dirtyRect);
    CGContextSaveGState(ctx);                                          /* left unbalanced on purpose in mode 1: the host must clean the clip stack up */
    CGContextClipToRect(ctx, CGRectMake(1, 1, 10, 10));
    if (mode == 1) {
        [[self window] close];                                          /* the window and its surface vanish during the repaint ... */
        CGContextSetRGBFillColor(ctx, 1, 0, 0, 1);                      /* ... and the context handed to this pass is still usable until it returns */
        CGContextFillRect(ctx, CGRectMake(0, 0, 5, 5));
    } else {
        CGContextRestoreGState(ctx);
    }
    if (mode == 2) {
        if (kept) CGContextRelease(kept);
        kept = CGContextRetain(ctx);
        [self setNeedsDisplay:YES];                                     /* ignored while this view is being drawn: no endless repaint */
    }
}
@end

static NSWindow *make_window(Probe **view) {
    NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 120, 90)
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    Probe *v = [[Probe alloc] initWithFrame:NSMakeRect(0, 0, 120, 90)];
    [[w contentView] addSubview:v];
    *view = v;
    return w;
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        [NSApplication sharedApplication];

        /* A */
        for (int i = 0; i < 40; i++) {
            @autoreleasepool {
                Probe *v = nil;
                NSWindow *w = make_window(&v);
                [w makeKeyAndOrderFront:nil];
                [v display];
                [v setNeedsDisplay:YES];
                [v displayIfNeeded];
                [v display];
                [w close];
            }
        }
        dA = draws;
        CHECK(dA >= 120);                                               /* the offscreen backend paints exactly once per request, Windows may add its own WM_PAINT */

        /* B */
        draws = 0;
        mode = 1;
        {
            Probe *v = nil;
            NSWindow *w = make_window(&v);
            [w makeKeyAndOrderFront:nil];
            [v display];
            dB = draws;
            CHECK(dB >= 1);
        }

        /* C */
        draws = 0;
        mode = 2;
        {
            Probe *v = nil;
            NSWindow *w = make_window(&v);
            [w makeKeyAndOrderFront:nil];
            [v display];
            dC = draws;
            CHECK(dC >= 1);
            CHECK(![v needsDisplay]);                                   /* the request made from inside drawRect: was ignored */
            CHECK(kept != NULL);
            CGContextFillRect(kept, CGRectMake(0, 0, 50, 50));          /* stale: refused by the host */
            CGContextSaveGState(kept);
            CGContextRelease(kept);                                     /* releasing the retained, now inert, context is fine */
            kept = NULL;
            [w close];
        }
    }
    NSLog(@"cg_lifecycle draws A=%d B=%d C=%d", dA, dB, dC);
    NSLog(@"cg_lifecycle done: failures=%d", failures);
    return failures;
}
