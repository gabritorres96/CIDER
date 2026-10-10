/* Fixture: gettimeofday, clock_gettime (Darwin clock ids), time and nanosleep. */
#include "darwin.h"

int main(void) {
    struct timeval tv;
    if (gettimeofday(&tv, 0) != 0) return 10;
    time_t t = time(0);
    if (t - tv.tv_sec > 2 || tv.tv_sec - t > 2) return 11;
    struct timespec rt;
    if (clock_gettime(CLOCK_REALTIME, &rt) != 0) return 12;
    if (rt.tv_sec - t > 2 || t - rt.tv_sec > 2 || rt.tv_nsec < 0 || rt.tv_nsec >= 1000000000L) return 13;
    struct timespec a, b;
    if (clock_gettime(CLOCK_MONOTONIC, &a) != 0) return 14;
    struct timespec req = {0, 60 * 1000 * 1000};             /* 60 ms */
    if (nanosleep(&req, 0) != 0) return 15;
    if (clock_gettime(CLOCK_MONOTONIC, &b) != 0) return 16;
    long ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
    if (ms < 50 || ms > 3000) return 17;
    if (clock_gettime(99, &a) != -1 || errno != EINVAL) return 18;       /* unknown clock id */
    struct timespec bad = {0, 2000000000L};
    if (nanosleep(&bad, 0) != -1 || errno != EINVAL) return 19;
    printf("time=%ld\n", t);
    return 0;
}
