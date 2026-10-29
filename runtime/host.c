#include "host.h"

#ifdef _WIN32
#include <windows.h>
#include <errno.h>
#include <stdlib.h>

int clock_gettime(int clock, struct timespec *value) {
    if (!value) {
        errno = EINVAL;
        return -1;
    }
    if (clock == CLOCK_MONOTONIC) {
        LARGE_INTEGER count, frequency;
        if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&count)) {
            errno = EIO;
            return -1;
        }
        value->tv_sec = (time_t)(count.QuadPart / frequency.QuadPart);
        value->tv_nsec = (long)((count.QuadPart % frequency.QuadPart) * INT64_C(1000000000) / frequency.QuadPart);
    } else if (clock == CLOCK_THREAD_CPUTIME_ID) {
        FILETIME created, exited, kernel, user;
        if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
            errno = EIO;
            return -1;
        }
        uint64_t ticks = ((uint64_t)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime;
        ticks += ((uint64_t)user.dwHighDateTime << 32) | user.dwLowDateTime;
        value->tv_sec = (time_t)(ticks / UINT64_C(10000000));
        value->tv_nsec = (long)(ticks % UINT64_C(10000000)) * 100;
    } else {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int nanosleep(const struct timespec *duration, struct timespec *remaining) {
    if (!duration || duration->tv_sec < 0 || duration->tv_nsec < 0 || duration->tv_nsec >= 1000000000) {
        errno = EINVAL;
        return -1;
    }
    uint64_t ticks = (uint64_t)duration->tv_sec * UINT64_C(10000000) + (duration->tv_nsec + 99) / 100;
    if (ticks) {
        HANDLE timer = CreateWaitableTimerW(NULL, TRUE, NULL);
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)ticks;
        if (!timer) {
            errno = EIO;
            return -1;
        }
        BOOL armed = SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
        DWORD waited = armed ? WaitForSingleObject(timer, INFINITE) : WAIT_FAILED;
        CloseHandle(timer);
        if (waited != WAIT_OBJECT_0) {
            errno = EIO;
            return -1;
        }
    }
    if (remaining)
        *remaining = (struct timespec){0};
    return 0;
}

int clock_nanosleep(int clock, int flags, const struct timespec *value, struct timespec *remaining) {
    if (clock != CLOCK_MONOTONIC || (flags != 0 && flags != TIMER_ABSTIME) || !value ||
        value->tv_sec < 0 || value->tv_nsec < 0 || value->tv_nsec >= 1000000000)
        return EINVAL;
    struct timespec delay = *value;
    if (flags == TIMER_ABSTIME) {
        struct timespec now;
        if (clock_gettime(clock, &now))
            return errno;
        delay.tv_sec -= now.tv_sec;
        delay.tv_nsec -= now.tv_nsec;
        if (delay.tv_nsec < 0) {
            delay.tv_sec--;
            delay.tv_nsec += 1000000000;
        }
        if (delay.tv_sec < 0)
            return 0;
    }
    return nanosleep(&delay, remaining) ? errno : 0;
}

int setenv(const char *name, const char *value, int overwrite) {
    if (!overwrite && getenv(name))
        return 0;
    int error = _putenv_s(name, value);
    if (error)
        errno = error;
    return error ? -1 : 0;
}
#endif
