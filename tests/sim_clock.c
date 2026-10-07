/* The system clock calls of timekeep.cpp. run.sh compiles timekeep.cpp with
 * gettimeofday, settimeofday and time renamed to these; they live in C, apart
 * from the system's own declarations, which differ between C libraries. */
#include <stdint.h>
#include <sys/time.h>
#include <time.h>

int64_t sim_clock_now_us(void);   /* timekeep_sim.cpp: the simulated system clock */
void sim_clock_set_us(int64_t us);

int sim_gettimeofday(struct timeval *tv, void *tz) {
    const int64_t us = sim_clock_now_us();
    (void)tz;
    tv->tv_sec = (time_t)(us / 1000000);
    tv->tv_usec = (suseconds_t)(us % 1000000);
    return 0;
}

int sim_settimeofday(const struct timeval *tv, const void *tz) {
    (void)tz;
    sim_clock_set_us((int64_t)tv->tv_sec * 1000000 + tv->tv_usec);
    return 0;
}

time_t sim_time(time_t *t) {
    const time_t s = (time_t)(sim_clock_now_us() / 1000000);
    if (t) *t = s;
    return s;
}
