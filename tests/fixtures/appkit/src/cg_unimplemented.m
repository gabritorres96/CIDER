/* A program that imports a real CoreGraphics function CIDER does not implement (CGContextDrawLinearGradient). It must be refused when it is loaded, naming the
   symbol and the library; it must never start and never receive a fake that does nothing. */
#include "AppKit/AppKit.h"

int main(int argc, const char *argv[]) {
    NSLog(@"this line must never run");
    CGContextDrawLinearGradient(NULL, NULL, CGPointMake(0, 0), CGPointMake(1, 1), 0);
    return 0;
}
