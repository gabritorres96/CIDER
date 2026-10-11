/* Fixture J: needs fork(), which libSystem exports (declared in the stub) but ORVYN does not implement. */
extern int fork(void);
int main(void) { return fork(); }
