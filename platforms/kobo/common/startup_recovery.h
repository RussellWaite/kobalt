#ifndef KOBALT_STARTUP_RECOVERY_H
#define KOBALT_STARTUP_RECOVERY_H
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
// Only unfinished Kobalt initialization counts, never a later reader crash.
bool kobalt_startup_remove(const char *path);
bool kobalt_startup_begin(const char *path);
bool kobalt_startup_complete(const char *path);
#ifdef __cplusplus
}
#endif
#endif
