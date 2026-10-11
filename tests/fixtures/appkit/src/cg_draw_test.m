/* ORVYN 0.6 reference drawing program: a window "ORVYN — CoreGraphics Drawing" whose custom view (DrawingView) draws a deterministic scene with CoreGraphics
   from -drawRect:. All drawing decisions are made here, in the Mach-O; the host only provides CoreGraphics, the graphics context and the window.

   Scene, in the DrawingView's own coordinates (400 x 300, origin bottom-left unless run with the argument "flipped"):
     background     (1.0, 1.0, 0.8)  whole view (the dirty rectangle must equal the view bounds)
     state rect     (20, 20, 80, 40)  red in state 0, blue in state 1 (toggled by the Redraw button)
     blue rect      (300, 240, 80, 40) blue
     diagonal       black, width 4, from (120, 20) to (280, 140)
     ellipse        green, in (150, 150, 120, 80)
     path figure    orange triangle (30,100) (90,100) (60,160) built with CGPath
     transformed    purple square (-20,-20,40,40) translated to (320,100) and rotated 45 degrees through a CGAffineTransform
     clipped        cyan rectangle (100,200,200,100) clipped to (130,245,110,45)
     stroked rect   black outline, width 6, (310,150,60,50)
   The program checks the arguments of -drawRect: (the NSRect passed on the stack) and logs one line per drawing pass. Script (offscreen backend, see test_cg_rendering.cpp):
     snapshot, click Redraw, snapshot ... */
#include "AppKit/AppKit.h"

#define PI 3.14159265358979323846

static BOOL g_flipped = NO;
static int g_draws = 0, g_failures = 0;
static int g_state = 0;
static NSInteger g_redraw_clicks = 0;

@interface DrawingView : NSView
@end

@implementation DrawingView

- (BOOL)isFlipped { return g_flipped; }

- (void)drawRect:(NSRect)dirtyRect {
    g_draws++;
    /* ABI check of the struct passed by value on the stack */
    NSRect bounds = [self bounds];
    if (dirtyRect.origin.x != 0 || dirtyRect.origin.y != 0 || dirtyRect.size.width != bounds.size.width || dirtyRect.size.height != bounds.size.height) {
        g_failures++;
        NSLog(@"FAIL dirtyRect = %d %d %d %d", (int)dirtyRect.origin.x, (int)dirtyRect.origin.y, (int)dirtyRect.size.width, (int)dirtyRect.size.height);
    }
    NSGraphicsContext *gc = [NSGraphicsContext currentContext];
    CGContextRef ctx = [gc CGContext];
    if (gc == nil || ctx == NULL) { g_failures++; NSLog(@"FAIL no graphics context"); return; }
    if ([gc isFlipped] != g_flipped) { g_failures++; NSLog(@"FAIL isFlipped"); }

    /* CGContextGetCTM returns a 48-byte struct through a hidden pointer; translate then scale composes as scale-after-translate in user space */
    {
        CGContextSaveGState(ctx);
        CGAffineTransform before = CGContextGetCTM(ctx);
        CGContextTranslateCTM(ctx, 5, 7);
        CGContextScaleCTM(ctx, 2, 3);
        CGAffineTransform after = CGContextGetCTM(ctx);
        CGAffineTransform expected = CGAffineTransformConcat(CGAffineTransformConcat(CGAffineTransformMakeScale(2, 3), CGAffineTransformMakeTranslation(5, 7)), before);
        if (!CGAffineTransformEqualToTransform(after, expected)) { g_failures++; NSLog(@"FAIL CTM composition"); }
        CGContextRestoreGState(ctx);
        if (!CGAffineTransformEqualToTransform(CGContextGetCTM(ctx), before)) { g_failures++; NSLog(@"FAIL CTM not restored"); }
    }

    /* background through a CGColor created from a colour space */
    CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
    const CGFloat bgc[4] = { 1.0, 1.0, 0.8, 1.0 };
    CGColorRef bg = CGColorCreate(rgb, bgc);
    if (CGColorGetNumberOfComponents(bg) != 4 || CGColorGetComponents(bg)[2] != 0.8) { g_failures++; NSLog(@"FAIL color components"); }
    CGContextSetFillColorWithColor(ctx, bg);
    CGContextFillRect(ctx, dirtyRect);            /* the NSRect argument, passed on the stack */
    CGColorRelease(bg);
    CGColorSpaceRelease(rgb);

    /* red / blue rectangles */
    if (g_state == 0) CGContextSetRGBFillColor(ctx, 1, 0, 0, 1); else CGContextSetRGBFillColor(ctx, 0, 0, 1, 1);
    CGContextFillRect(ctx, CGRectMake(20, 20, 80, 40));
    CGContextSetRGBFillColor(ctx, 0, 0, 1, 1);
    CGContextFillRect(ctx, CGRectMake(300, 240, 80, 40));

    /* diagonal line */
    CGContextSetRGBStrokeColor(ctx, 0, 0, 0, 1);
    CGContextSetLineWidth(ctx, 4);
    CGContextBeginPath(ctx);
    CGContextMoveToPoint(ctx, 120, 20);
    CGContextAddLineToPoint(ctx, 280, 140);
    CGContextStrokePath(ctx);

    /* green ellipse */
    CGContextSetRGBFillColor(ctx, 0, 1, 0, 1);
    CGContextBeginPath(ctx);
    CGContextAddEllipseInRect(ctx, CGRectMake(150, 150, 120, 80));
    CGContextFillPath(ctx);

    /* a CGPath figure */
    CGMutablePathRef tri = CGPathCreateMutable();
    CGPathMoveToPoint(tri, NULL, 30, 100);
    CGPathAddLineToPoint(tri, NULL, 90, 100);
    CGPathAddLineToPoint(tri, NULL, 60, 160);
    CGPathCloseSubpath(tri);
    CGContextSetRGBFillColor(ctx, 1, 0.5, 0, 1);
    CGContextAddPath(ctx, tri);
    CGContextFillPath(ctx);
    CGPathRelease(tri);

    /* a transformed figure: a 48-byte CGAffineTransform passed by value */
    CGContextSaveGState(ctx);
    CGAffineTransform t = CGAffineTransformMakeTranslation(320, 100);
    t = CGAffineTransformRotate(t, PI / 4);
    CGContextConcatCTM(ctx, t);
    CGContextSetRGBFillColor(ctx, 0.5, 0, 0.5, 1);
    CGContextFillRect(ctx, CGRectMake(-20, -20, 40, 40));
    CGContextRestoreGState(ctx);

    /* a clipped figure */
    CGContextSaveGState(ctx);
    CGContextClipToRect(ctx, CGRectMake(130, 245, 110, 45));
    CGContextSetRGBFillColor(ctx, 0, 1, 1, 1);
    CGContextFillRect(ctx, CGRectMake(100, 200, 200, 100));
    CGContextRestoreGState(ctx);

    /* stroked rectangle */
    CGContextSetRGBStrokeColor(ctx, 0, 0, 0, 1);
    CGContextSetLineWidth(ctx, 6);
    CGContextStrokeRect(ctx, CGRectMake(310, 150, 60, 50));
}
@end

@interface Controller : NSObject <NSApplicationDelegate, NSWindowDelegate> {
    NSWindow *window;
    DrawingView *drawing;
}
- (void)build;
- (void)show;
- (void)redraw:(id)sender;
@end

@implementation Controller

- (void)build {
    window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 400, 340)
                                         styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                                           backing:NSBackingStoreBuffered
                                             defer:NO];
    [window setTitle:@"ORVYN — CoreGraphics Drawing"];
    [window setDelegate:self];
    NSView *content = [window contentView];
    /* the drawing view fills the area above the button strip and follows the window size */
    drawing = [[DrawingView alloc] initWithFrame:NSMakeRect(0, 40, 400, 300)];
    [drawing setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [content addSubview:drawing];
    NSButton *redraw = [[NSButton alloc] initWithFrame:NSMakeRect(10, 6, 120, 28)];
    [redraw setTitle:@"Redraw"];
    [redraw setTarget:self];
    [redraw setAction:@selector(redraw:)];
    [content addSubview:redraw];
    [window center];
}

- (void)show { [window makeKeyAndOrderFront:nil]; }

- (void)redraw:(id)sender {
    g_redraw_clicks++;
    g_state = 1 - g_state;                     /* the state lives in this program; the host only repaints when asked */
    [drawing setNeedsDisplay:YES];
}

- (void)applicationDidFinishLaunching:(NSNotification *)n { NSLog(@"cg_draw_test launched (flipped=%d)", (int)g_flipped); }
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender { return NO; }
/* The run loop is stopped (not exit()) so that main returns normally and ARC releases everything: the host can then prove nothing leaked. */
- (void)windowWillClose:(NSNotification *)n {
    NSLog(@"cg_draw_test done: draws=%d redraws=%d failures=%d", g_draws, (int)g_redraw_clicks, g_failures);
    [NSApp stop:nil];
}
@end

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        for (int i = 1; i < argc; i++) {
            if (argv[i][0] == 'f' && argv[i][1] == 'l') g_flipped = YES;     /* "flipped" */
        }
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        Controller *controller = [[Controller alloc] init];
        [app setDelegate:controller];
        [controller build];
        [controller show];
        [app activateIgnoringOtherApps:YES];
        [app run];
    }
    return g_failures;
}
