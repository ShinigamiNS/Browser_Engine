/* pthread_stub.h
   Minimal pthread stubs for single-threaded QuickJS build on Windows/MinGW.
   QuickJS only uses pthread for SharedArrayBuffer Atomics.wait() which we
   don't need in a browser engine. All operations become no-ops. */

#ifndef PTHREAD_STUB_H
#define PTHREAD_STUB_H

#include <windows.h>
#include <time.h>
#include <malloc.h>   /* _msize, _alloca */

/* ETIMEDOUT must come before it's used below */
#ifndef ETIMEDOUT
#define ETIMEDOUT 110
#endif

/* CONFIG_VERSION - defined here so we don't need shell quoting tricks */
#ifndef CONFIG_VERSION
#define CONFIG_VERSION "1.0"
#endif

/* Types */
typedef CRITICAL_SECTION pthread_mutex_t;
typedef void*            pthread_cond_t;
typedef void*            pthread_t;
typedef void*            pthread_attr_t;

/* Static mutex initializer */
#define PTHREAD_MUTEX_INITIALIZER {0}

/* Mutex ops — no-ops for single-threaded use */
static inline int pthread_mutex_init(pthread_mutex_t *m, void *a)    { (void)m;(void)a; return 0; }
static inline int pthread_mutex_destroy(pthread_mutex_t *m)           { (void)m; return 0; }
static inline int pthread_mutex_lock(pthread_mutex_t *m)              { (void)m; return 0; }
static inline int pthread_mutex_unlock(pthread_mutex_t *m)            { (void)m; return 0; }

/* Condition variable — stubbed (Atomics.wait not needed) */
static inline int pthread_cond_init(pthread_cond_t *c, void *a)      { (void)c;(void)a; return 0; }
static inline int pthread_cond_destroy(pthread_cond_t *c)             { (void)c; return 0; }
static inline int pthread_cond_wait(pthread_cond_t *c,
                                     pthread_mutex_t *m)              { (void)c;(void)m; return 0; }
static inline int pthread_cond_timedwait(pthread_cond_t *c,
                                          pthread_mutex_t *m,
                                          const struct timespec *t)   { (void)c;(void)m;(void)t; return ETIMEDOUT; }
static inline int pthread_cond_signal(pthread_cond_t *c)              { (void)c; return 0; }
static inline int pthread_cond_broadcast(pthread_cond_t *c)           { (void)c; return 0; }

/* clock_gettime */
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif
static inline int clock_gettime(int clk, struct timespec *ts) {
    FILETIME ft;
    ULONGLONG t;
    (void)clk;
    GetSystemTimeAsFileTime(&ft);
    t = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    t -= 116444736000000000ULL;
    ts->tv_sec  = (long)(t / 10000000ULL);
    ts->tv_nsec = (long)((t % 10000000ULL) * 100);
    return 0;
}

/* alloca */
#ifndef alloca
#define alloca _alloca
#endif

#endif /* PTHREAD_STUB_H */
