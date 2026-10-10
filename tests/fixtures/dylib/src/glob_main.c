/* Exported data symbol shared in both directions (bound through a GOT slot). */
extern int cider_global_value;
extern int cider_read_global(void);
int main(void) {
    if (cider_read_global() != 40) return 1;
    cider_global_value += 2;                       /* the executable writes the library's variable */
    return cider_read_global();                    /* the library sees 42 */
}
