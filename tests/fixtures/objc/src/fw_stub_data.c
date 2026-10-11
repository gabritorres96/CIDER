/* A DATA import of a recognised-but-unimplemented framework (Carbon's kTISPropertyLocalizedName): data cannot be stubbed, so the link must fail. */
extern void *kTISPropertyLocalizedName;
int main(void) { return kTISPropertyLocalizedName ? 1 : 0; }
