/* Exported data symbol shared in both directions (bound through a GOT slot). */
extern int orvyn_global_value;
extern int orvyn_read_global(void);
int main(void) {
    if (orvyn_read_global() != 40) return 1;
    orvyn_global_value += 2;                       /* the executable writes the library's variable */
    return orvyn_read_global();                    /* the library sees 42 */
}
