#pragma once
// Force-included into every firmware translation unit (see CMakeLists.txt).
// Supplies the newlib/POSIX bits the firmware relies on that MinGW lacks, and a
// device clock (settimeofday / POSIX TZ strings) that is separate from the host's.
#include <stddef.h>
#include <time.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif
size_t strlcpy(char* dst, const char* src, size_t n);
size_t strlcat(char* dst, const char* src, size_t n);

time_t sim_time(time_t* t);
int sim_gettimeofday(struct timeval* tv, void* tz);
int sim_settimeofday(const struct timeval* tv, const void* tz);
int sim_setenv(const char* name, const char* value, int overwrite);
void sim_tzset(void);
struct tm* sim_localtime_r(const time_t* t, struct tm* out);
struct tm* sim_localtime(const time_t* t);
struct tm* sim_gmtime_r(const time_t* t, struct tm* out);
time_t sim_mktime(struct tm* tm);
time_t sim_timegm(struct tm* tm);
#ifdef __cplusplus
}
#endif

#ifndef SIM_LIBC_IMPL
#define time(t) sim_time(t)
#define gettimeofday sim_gettimeofday
#define settimeofday sim_settimeofday
#define setenv sim_setenv
#define tzset sim_tzset
#define localtime_r sim_localtime_r
#define localtime sim_localtime
#define gmtime_r sim_gmtime_r
#define mktime sim_mktime
#define timegm sim_timegm
#endif
