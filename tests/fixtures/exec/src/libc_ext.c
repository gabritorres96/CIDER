/* libSystem functions added for the first external binary (jq): libm beyond the basics (including the mixed int/double and x87 signatures), time
   conversion in UTC, strftime/strptime, strerror, memmem/strspn, setlocale (C only), rand, realpath/pathconf/isatty/dirname, the rune table and
   ___maskrune. Every expected value is a mathematical or calendar fact (not something ORVYN printed). Exit code 42 = all good, else the failing check. */
#include "darwin.h"

double acos(double), acosh(double), asin(double), asinh(double), atanh(double), cbrt(double), cosh(double), sinh(double), tanh(double), erf(double), erfc(double);
double exp2(double), expm1(double), log1p(double), log2(double), logb(double), tgamma(double), lgamma(double), j0(double), j1(double), y0(double), y1(double);
double fdim(double, double), hypot(double, double), nextafter(double, double), remainder(double, double), scalb(double, double), scalbln(double, long);
double jn(int, double), yn(int, double), lgamma_r(double, int *), nexttoward(double, long double);
void *memmem(const void *, size_t, const void *, size_t);
size_t strspn(const char *, const char *);
char *strerror(int);
char *setlocale(int, const char *);
char *dirname(char *);
char *realpath(const char *, char *) __asm("_realpath$DARWIN_EXTSN");
long pathconf(const char *, int);
int isatty(int);
int rand(void);
int memcmp(const void *, const void *, size_t);
int strcmp(const char *, const char *);
size_t strlen(const char *);
struct tm { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; long tm_gmtoff; char *tm_zone; };
struct tm *gmtime_r(const long *, struct tm *);
struct tm *localtime_r(const long *, struct tm *);
long timegm(struct tm *);
size_t strftime(char *, size_t, const char *, const struct tm *);
char *strptime(const char *, const char *, struct tm *);
extern const unsigned char _DefaultRuneLocale[];
int __maskrune(int, unsigned long);

static int near(double a, double b, double tol) { double d = a - b; return (d < 0 ? -d : d) <= tol; }

int main(void) {
    /* libm: exact or well-known values */
    if (!near(acos(1.0), 0.0, 0) || !near(asin(1.0) * 2, 3.14159265358979, 1e-12)) return 1;
    if (!near(acosh(1.0), 0.0, 0) || !near(asinh(0.0), 0.0, 0) || !near(atanh(0.0), 0.0, 0)) return 2;
    if (cbrt(27.0) != 3.0 || cbrt(-8.0) != -2.0) return 3;
    if (cosh(0.0) != 1.0 || sinh(0.0) != 0.0 || tanh(0.0) != 0.0) return 4;
    if (erf(0.0) != 0.0 || erfc(0.0) != 1.0) return 5;
    if (exp2(10.0) != 1024.0 || log2(1024.0) != 10.0 || expm1(0.0) != 0.0 || log1p(0.0) != 0.0) return 6;
    if (logb(8.0) != 3.0 || logb(0.75) != -1.0) return 7;
    if (tgamma(5.0) != 24.0 || tgamma(1.0) != 1.0) return 8;
    if (!near(lgamma(1.0), 0.0, 1e-15) || !near(lgamma(3.0), 0.6931471805599453, 1e-12)) return 9;   /* ln(2!) */
    int sign = 0;
    if (!near(lgamma_r(-0.5, &sign), 1.2655121234846454, 1e-12) || sign != -1) return 10;           /* Gamma(-1/2) = -2 sqrt(pi): negative */
    if (!near(lgamma_r(2.5, &sign), 0.2846828704729192, 1e-12) || sign != 1) return 11;
    if (j0(0.0) != 1.0 || j1(0.0) != 0.0 || jn(0, 0.0) != 1.0 || jn(2, 0.0) != 0.0) return 12;      /* int + double argument classes */
    if (!near(y0(1.0), 0.08825696421567697, 1e-12) || !near(y1(1.0), -0.7812128213002887, 1e-12) || !near(yn(2, 1.0), -1.6506826068162546, 1e-12)) return 13;
    if (fdim(5.0, 3.0) != 2.0 || fdim(3.0, 5.0) != 0.0) return 14;
    if (hypot(3.0, 4.0) != 5.0) return 15;
    if (remainder(10.0, 3.0) != 1.0 || remainder(11.0, 3.0) != -1.0) return 16;                      /* IEEE: nearest integer quotient */
    if (scalb(3.0, 4.0) != 48.0 || scalbln(3.0, 4) != 48.0 || scalbln(1.0, -2) != 0.25) return 17;
    if (!(nextafter(1.0, 2.0) > 1.0) || nextafter(1.0, 2.0) - 1.0 != 2.220446049250313e-16 || nextafter(1.0, 0.0) >= 1.0) return 18;
    if (nexttoward(1.0, 2.0L) != nextafter(1.0, 2.0)) return 19;                                     /* long double argument: x87 value in memory */
    if (nexttoward(1.0, 1.0L) != 1.0) return 20;
    if (nexttoward(1.0, 1.0L + 1e-18L) != nextafter(1.0, 2.0)) return 21;                           /* y one extended-precision step above x: still "above" */
    if (nexttoward(1.0, 0.0L) >= 1.0) return 22;

    /* strings */
    const char *hay = "abcabcabd";
    if (memmem(hay, 9, "cab", 3) != hay + 2 || memmem(hay, 9, "abd", 3) != hay + 6 || memmem(hay, 9, "xyz", 3) != NULL || memmem(hay, 2, "abc", 3) != NULL) return 23;
    if (memmem(hay, 9, "", 0) != hay) return 24;
    if (strspn("aabbcc", "ab") != 4 || strspn("xyz", "ab") != 0 || strspn("", "ab") != 0) return 25;
    if (strcmp(strerror(2), "No such file or directory") != 0 || strcmp(strerror(13), "Permission denied") != 0 || strcmp(strerror(22), "Invalid argument") != 0) return 26;
    if (strcmp(strerror(9999), "Unknown error: 9999") != 0) return 27;
    char d1[] = "/usr/lib/x", d2[] = "file", d3[] = "/", d4[] = "a/b/", d5[] = "";
    if (strcmp(dirname(d1), "/usr/lib") != 0 || strcmp(dirname(d2), ".") != 0 || strcmp(dirname(d3), "/") != 0 || strcmp(dirname(d4), "a") != 0 || strcmp(dirname(d5), ".") != 0) return 28;

    /* locale: only "C" */
    if (strcmp(setlocale(6, ""), "C") != 0 || strcmp(setlocale(6, "C"), "C") != 0 || strcmp(setlocale(6, "POSIX"), "C") != 0) return 29;
    if (setlocale(6, "de_DE.UTF-8") != NULL) return 30;

    /* time, UTC: 1425599507 = 2015-03-05 23:51:47 Thursday, day 63 of the year (the jq manual's own example) */
    long t = 1425599507;
    struct tm tm;
    if (gmtime_r(&t, &tm) != &tm) return 31;
    if (tm.tm_year != 115 || tm.tm_mon != 2 || tm.tm_mday != 5 || tm.tm_hour != 23 || tm.tm_min != 51 || tm.tm_sec != 47 || tm.tm_wday != 4 || tm.tm_yday != 63 || tm.tm_isdst != 0) return 32;
    if (tm.tm_gmtoff != 0 || strcmp(tm.tm_zone, "UTC") != 0) return 33;
    struct tm lt;
    localtime_r(&t, &lt);
    if (lt.tm_hour != 23 || lt.tm_gmtoff != 0) return 34;                                            /* the guest's zone is UTC */
    if (timegm(&tm) != t) return 35;
    long neg = -86400 * 365 - 1;                                                                     /* 1968-12-31 23:59:59 (before the epoch) */
    gmtime_r(&neg, &tm);
    if (tm.tm_year != 68 || tm.tm_mon != 11 || tm.tm_mday != 31 || tm.tm_hour != 23 || tm.tm_sec != 59 || tm.tm_wday != 2) return 36;
    struct tm leap = {0, 0, 0, 29, 1, 124, 0, 0, 0, 0, 0};                                           /* 2024-02-29 00:00:00 */
    long lt2 = timegm(&leap);
    if (lt2 != 1709164800 || leap.tm_wday != 4 || leap.tm_yday != 59) return 37;
    struct tm over = {0, 0, 0, 32, 0, 100, 0, 0, 0, 0, 0};                                           /* Jan 32 normalises to Feb 1 */
    timegm(&over);
    if (over.tm_mon != 1 || over.tm_mday != 1) return 38;
    char buf[128];
    gmtime_r(&t, &tm);
    size_t n = strftime(buf, sizeof buf, "%A, %B %d, %Y %H:%M:%S %j %a %b %e %y %p %I %Z %%", &tm);
    if (n == 0 || strcmp(buf, "Thursday, March 05, 2015 23:51:47 064 Thu Mar  5 15 PM 11 UTC %") != 0) return 39;
    if (strftime(buf, sizeof buf, "%F %T %D %R", &tm) == 0 || strcmp(buf, "2015-03-05 23:51:47 03/05/15 23:51") != 0) return 40;
    if (strftime(buf, sizeof buf, "%s", &tm) == 0 || strcmp(buf, "1425599507") != 0) return 41;
    if (strftime(buf, sizeof buf, "%V %G %u %w %U %W", &tm) == 0 || strcmp(buf, "10 2015 4 4 09 09") != 0) return 42;     /* ISO week 10 of 2015 */
    if (strftime(buf, 5, "%Y-%m-%d", &tm) != 0) return 43;                                           /* does not fit: 0 */
    struct tm p;
    for (int i = 0; i < (int)sizeof p; ++i) ((char *)&p)[i] = 0;
    const char *end = strptime("2015-03-05T23:51:47Z", "%Y-%m-%dT%H:%M:%SZ", &p);
    if (!end || *end != 0 || p.tm_year != 115 || p.tm_mon != 2 || p.tm_mday != 5 || p.tm_hour != 23 || p.tm_min != 51 || p.tm_sec != 47) return 44;
    for (int i = 0; i < (int)sizeof p; ++i) ((char *)&p)[i] = 0;
    end = strptime("Thursday March 5 2015 11:51:47 PM", "%A %B %d %Y %I:%M:%S %p", &p);
    if (!end || *end != 0 || p.tm_wday != 4 || p.tm_mon != 2 || p.tm_hour != 23) return 45;
    for (int i = 0; i < (int)sizeof p; ++i) ((char *)&p)[i] = 0;
    end = strptime("2015 64", "%Y %j", &p);
    if (!end || p.tm_yday != 63 || p.tm_mon != 2 || p.tm_mday != 5) return 46;                       /* day of year -> month and day */
    for (int i = 0; i < (int)sizeof p; ++i) ((char *)&p)[i] = 0;
    if (strptime("2015-13-01", "%Y-%m-%d", &p) != NULL) return 47;                                   /* month 13 */
    if (strptime("abc", "%Y", &p) != NULL) return 48;
    end = strptime("07/04/21 rest", "%D", &p);
    if (!end || strcmp(end, " rest") != 0 || p.tm_year != 121 || p.tm_mon != 6 || p.tm_mday != 4) return 49;

    /* rand: Park-Miller from seed 1: 16807, 282475249, 1622650073, 984943658, 1144108930 (the minimal-standard sequence) */
    if (rand() != 16807 || rand() != 282475249 || rand() != 1622650073 || rand() != 984943658 || rand() != 1144108930) return 50;

    /* files: confined to the guest root */
    char *rp = realpath("/", NULL);
    if (!rp || strcmp(rp, "/") != 0) return 51;
    if (realpath("/definitely/not/here", NULL) != NULL || errno != ENOENT) return 52;
    if (pathconf("/", 4) != 255 || pathconf("/", 5) != 1024) return 53;
    if (pathconf("/", 9999) != -1 || errno != EINVAL) return 54;
    if (pathconf("/not/here", 4) != -1 || errno != ENOENT) return 55;
    if (isatty(1) != 0 || errno != 25) return 56;                                                     /* captured stdout is not a terminal; errno ENOTTY(25) */
    if (isatty(77) != 0 || errno != EBADF) return 57;

    /* ctype: the table layout the inlined macros use (offset 60, 4 bytes per entry) and ___maskrune */
    const unsigned *rt = (const unsigned *)(_DefaultRuneLocale + 60);
    if (!(rt['a'] & 0x100) || !(rt['Z'] & 0x100) || (rt['1'] & 0x100) || !(rt[' '] & 0x4000) || !(rt['\n'] & 0x4000) || (rt['x'] & 0x4000)) return 58;
    if (!(rt['5'] & 0x400) || !(rt['f'] & 0x10000) || (rt['g'] & 0x10000) || !(rt['a'] & 0x1000) || !(rt['A'] & 0x8000) || !(rt['!'] & 0x2000)) return 59;
    if (__maskrune('a', 0x100) == 0 || __maskrune('1', 0x100) != 0 || __maskrune(200, 0x100) != 0 || __maskrune(-1, 0x100) != 0) return 60;
    printf("libc ext ok\n");
    return 42;
}
