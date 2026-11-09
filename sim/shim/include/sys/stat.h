#pragma once
// Wraps the system sys/stat.h so stat() on "/littlefs/..." / "/sd/..." paths
// resolves inside the simulated device's data folder.
#include_next <sys/stat.h>

#ifdef __cplusplus
extern "C" {
#endif
int sim_stat(const char* path, struct stat* st);
#ifdef __cplusplus
}
#endif

#ifndef SIM_LIBC_IMPL
#define stat(p, s) sim_stat(p, s)
#endif
