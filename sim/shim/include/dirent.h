#pragma once
// POSIX directory API over the simulated VFS ("/littlefs/..." and "/sd/..." map to
// the device's data folders). Replaces MinGW's dirent.h for firmware code.
#include <stddef.h>

#define DT_UNKNOWN 0
#define DT_DIR 4
#define DT_REG 8

struct dirent {
    unsigned short d_ino;
    unsigned char d_type;
    char d_name[256];
};
typedef struct sim_DIR DIR;

#ifdef __cplusplus
extern "C" {
#endif
DIR* sim_opendir(const char* path);
struct dirent* sim_readdir(DIR* d);
int sim_closedir(DIR* d);
void sim_rewinddir(DIR* d);
#ifdef __cplusplus
}
#endif

#define opendir sim_opendir
#define readdir sim_readdir
#define closedir sim_closedir
#define rewinddir sim_rewinddir
