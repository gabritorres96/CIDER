/* double arguments/returns cross the System V -> Win64 bridge (xmm0/xmm1). */
extern double sqrt(double x);
extern double pow(double a, double b);
extern double floor(double x);
volatile double g_x = 144.0;
int main(void) {
    double r = sqrt(g_x) + pow(2.0, 5.0) + floor(2.9);   /* 12 + 32 + 2 = 46 */
    return (int)r;
}
