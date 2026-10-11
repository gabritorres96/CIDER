/* Neither the executable nor anything else but the two libraries defines orvyn_shared: the first library in load order (7) wins for both,
   so the result is 7 + 7 = 14 (an implementation without coalescing returns 7 + 9 = 16). */
extern int orvyn_a_get(void);
extern int orvyn_b_get(void);
int main(void) { return orvyn_a_get() + orvyn_b_get(); }
