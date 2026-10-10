/* Fixture: printf with strings and characters. */
#include "darwin.h"

int main(void) {
    printf("Text: %s\n", "CIDER");
    printf("[%10s][%-10s][%.3s][%10.2s]\n", "hello", "hello", "hello", "hello");
    const char *none = 0;
    printf("[%s][%8s]\n", none, none);
    printf("%s %s %s %s %s %s %s\n", "a", "b", "c", "d", "e", "f", "g");   /* the seventh string is on the stack */
    char tail = '!';
    printf("%c%c%c%c\n", 'o', 'k', ' ', tail);
    return 0;
}
