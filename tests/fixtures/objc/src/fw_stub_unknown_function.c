/* ApplicationServices exports AXUIElementPostKeyboardEvent, but ORVYN has no stub for it (and no implementation): the link must fail. */
extern int AXUIElementPostKeyboardEvent(void *element, unsigned short character, unsigned short virtual_key, unsigned char key_down);
int main(void) { return AXUIElementPostKeyboardEvent(0, 0, 0, 1); }
