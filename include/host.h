#ifndef RECOMP_HOST_H
#define RECOMP_HOST_H

/* Host compatibility used by the runtime, generated code and game adapters. */
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef _MSC_VER
#ifdef __cplusplus
#define _Thread_local thread_local
#else
#define _Thread_local __declspec(thread)
#define _Noreturn __declspec(noreturn)
#endif
#endif

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#include <sys/stat.h>
#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#endif
#define strdup _strdup
#define getpid _getpid
#define flockfile _lock_file
#define funlockfile _unlock_file
#define RECOMP_NULL_DEVICE "NUL"
#define CLOCK_MONOTONIC 1
#define CLOCK_THREAD_CPUTIME_ID 2
#define TIMER_ABSTIME 1
#ifdef __cplusplus
extern "C" {
#endif
int clock_gettime(int clock, struct timespec *value);
int nanosleep(const struct timespec *duration, struct timespec *remaining);
int clock_nanosleep(int clock, int flags, const struct timespec *value, struct timespec *remaining);
int setenv(const char *name, const char *value, int overwrite);
#ifdef __cplusplus
}
#endif
#else
#define RECOMP_NULL_DEVICE "/dev/null"
#endif

#endif
