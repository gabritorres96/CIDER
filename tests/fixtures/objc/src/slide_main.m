/* Non-fragile instance layout: Square's ivar offsets were computed against Shape v1 (16 bytes) but the dylib ships Shape v2 (24 bytes).
   The runtime must slide Square's offset variables; if it did not, `side` would overwrite Shape's `extra`. */
#include "shape_v1_header.h"
#include "darwin.h"

@interface Square : Shape {
    int side;
}
- (void)setSide:(int)s;
- (int)side;
@end
@implementation Square
- (void)setSide:(int)s { side = s; }
- (int)side { return side; }
@end

int main(void) {
    Square *sq = [Square new];
    [sq setSide:5];
    printf("area=%d side=%d extra=%ld size=%lu\n", [sq area], [sq side], [sq extra], class_getInstanceSize(objc_getClass("Square")));
    if ([sq extra] != 99) return 1;                         /* untouched by the subclass's ivar */
    if ([sq side] != 5) return 2;
    if (class_getInstanceSize(objc_getClass("Square")) != 32) return 3;     /* 24 (Shape v2) + int, rounded to 8 */
    Ivar side = class_getInstanceVariable(objc_getClass("Square"), "side");
    if (ivar_getOffset(side) != 24) return 4;               /* slid from 16 to 24 */
    int area = [sq area];
    [sq destroy];
    return area;                                            /* 42 */
}
