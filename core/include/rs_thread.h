// rs_thread.h — the threading primitives the live engine and the server rely on,
// on every platform the tree supports.
//
// On POSIX this is a one-line passthrough to <pthread.h>. On Windows it is a
// small shim over SRWLOCK / CONDITION_VARIABLE / _beginthreadex that keeps the
// pthread spelling, so live.c, server.c, net.c and rs_dash.c contain exactly one
// threading implementation rather than two. That choice is deliberate: the live
// engine is ~160 lock/unlock pairs of subtle ordering, and a second copy under
// #ifdef would be the kind of thing that works until it deadlocks at 3am on the
// platform nobody tests.
//
// Only the subset the tree actually uses is implemented, and each function
// matches pthread semantics for that subset:
//   mutex   init/lock/unlock/destroy  (non-recursive, like PTHREAD_MUTEX_NORMAL)
//   cond    init/wait/timedwait/broadcast/destroy
//   thread  create/join/detach
//   once    pthread_once
//   TLS     key_create (with destructor) / getspecific / setspecific
//   clock   clock_gettime(CLOCK_REALTIME | CLOCK_MONOTONIC)
//
// Deliberately absent: rwlocks, barriers, cancellation, pthread_attr_t, thread
// priorities, pthread_kill. Nothing here needs them, and a stub that silently
// does nothing is worse than a compile error.

#ifndef RS_THREAD_H
#define RS_THREAD_H

#ifndef _WIN32

#include <pthread.h>
#include <time.h>
#include <errno.h>

#else  // ---------------------------------------------------------------- Win32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>   // _beginthreadex
#include <errno.h>
#include <time.h>
#include <stdlib.h>
#include <stdint.h>   // uintptr_t, for _beginthreadex's return

#ifndef ETIMEDOUT      // MSVC's <errno.h> has had this since VS2010; MinGW too.
#define ETIMEDOUT 138
#endif

// ---- clock_gettime ---------------------------------------------------------
// CLOCK_REALTIME is wall time since the Unix epoch, which is what the cond
// deadlines below are expressed in. CLOCK_MONOTONIC is QPC, which is immune to
// the clock being stepped — metrics.c and script.c's timeout both want that.

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

typedef int rs_clockid_t;

static inline int rs_clock_gettime(rs_clockid_t id, struct timespec *ts) {
    if (!ts) return -1;
    if (id == CLOCK_MONOTONIC) {
        LARGE_INTEGER freq, ctr;
        if (!QueryPerformanceFrequency(&freq) || !QueryPerformanceCounter(&ctr)) return -1;
        ts->tv_sec  = (time_t)(ctr.QuadPart / freq.QuadPart);
        ts->tv_nsec = (long)(((ctr.QuadPart % freq.QuadPart) * 1000000000LL) / freq.QuadPart);
        return 0;
    }
    // FILETIME counts 100ns ticks from 1601-01-01; 11644473600s to the epoch.
    FILETIME ft;
    ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    unsigned long long t100 = u.QuadPart - 116444736000000000ULL;
    ts->tv_sec  = (time_t)(t100 / 10000000ULL);
    ts->tv_nsec = (long)((t100 % 10000000ULL) * 100ULL);
    return 0;
}
#define clock_gettime(id, ts) rs_clock_gettime((id), (ts))

// ---- mutex -----------------------------------------------------------------
// SRWLOCK, not CRITICAL_SECTION: it is a pointer-sized word needing no
// destruction, and like a default pthread mutex it is NOT recursive — relocking
// on the same thread deadlocks in both worlds, so a bug shows up identically.

typedef SRWLOCK pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT

static inline int pthread_mutex_init(pthread_mutex_t *m, const void *attr) {
    (void)attr;
    InitializeSRWLock(m);
    return 0;
}
static inline int pthread_mutex_lock(pthread_mutex_t *m)    { AcquireSRWLockExclusive(m); return 0; }
static inline int pthread_mutex_unlock(pthread_mutex_t *m)  { ReleaseSRWLockExclusive(m); return 0; }
static inline int pthread_mutex_trylock(pthread_mutex_t *m) { return TryAcquireSRWLockExclusive(m) ? 0 : EBUSY; }
static inline int pthread_mutex_destroy(pthread_mutex_t *m) { (void)m; return 0; }  // SRWLOCK needs none

// ---- condition variable ----------------------------------------------------

typedef CONDITION_VARIABLE pthread_cond_t;
#define PTHREAD_COND_INITIALIZER CONDITION_VARIABLE_INIT

static inline int pthread_cond_init(pthread_cond_t *c, const void *attr) {
    (void)attr;
    InitializeConditionVariable(c);
    return 0;
}
static inline int pthread_cond_destroy(pthread_cond_t *c)   { (void)c; return 0; }
static inline int pthread_cond_signal(pthread_cond_t *c)    { WakeConditionVariable(c); return 0; }
static inline int pthread_cond_broadcast(pthread_cond_t *c) { WakeAllConditionVariable(c); return 0; }

static inline int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    return SleepConditionVariableSRW(c, m, INFINITE, 0) ? 0 : EINVAL;
}

// pthread takes an ABSOLUTE CLOCK_REALTIME deadline; Win32 takes a RELATIVE
// millisecond count. Converting on every call is what makes live_wait() behave
// the same on both — including the case where the deadline has already passed,
// which must return ETIMEDOUT immediately rather than blocking forever.
static inline int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                                         const struct timespec *abstime) {
    struct timespec now;
    DWORD ms;
    long long delta_ms;
    if (!abstime) return pthread_cond_wait(c, m);
    rs_clock_gettime(CLOCK_REALTIME, &now);
    delta_ms = (long long)(abstime->tv_sec - now.tv_sec) * 1000LL +
               ((long long)abstime->tv_nsec - (long long)now.tv_nsec) / 1000000LL;
    if (delta_ms <= 0) return ETIMEDOUT;
    // Cap below INFINITE so a nonsense far-future deadline cannot become "wait
    // forever" — it stays a timed wait that the caller's loop will re-arm.
    ms = (delta_ms > 0x7FFFFFFFLL) ? 0x7FFFFFFFu : (DWORD)delta_ms;
    if (SleepConditionVariableSRW(c, m, ms, 0)) return 0;
    return (GetLastError() == ERROR_TIMEOUT) ? ETIMEDOUT : EINVAL;
}

// ---- threads ---------------------------------------------------------------
// _beginthreadex rather than CreateThread: the CRT needs the per-thread state it
// sets up, and the code below calls into libcurl and the CRT freely.

typedef HANDLE pthread_t;

typedef struct {
    void *(*fn)(void *);
    void *arg;
} rs_thread_start;

static inline unsigned __stdcall rs_thread_trampoline(void *raw) {
    rs_thread_start s = *(rs_thread_start *)raw;
    free(raw);
    s.fn(s.arg);   // return values are unused tree-wide; pthread_join takes NULL
    return 0;
}

static inline int pthread_create(pthread_t *t, const void *attr,
                                 void *(*fn)(void *), void *arg) {
    uintptr_t h;
    rs_thread_start *s;
    (void)attr;
    if (!t || !fn) return EINVAL;
    s = (rs_thread_start *)malloc(sizeof(*s));
    if (!s) return EAGAIN;
    s->fn = fn;
    s->arg = arg;
    h = _beginthreadex(NULL, 0, rs_thread_trampoline, s, 0, NULL);
    if (h == 0) { free(s); return EAGAIN; }
    *t = (HANDLE)h;
    return 0;
}

static inline int pthread_join(pthread_t t, void **retval) {
    if (retval) *retval = NULL;
    if (!t) return EINVAL;
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    return 0;
}

static inline int pthread_detach(pthread_t t) {
    if (t) CloseHandle(t);   // the thread runs on; we just stop tracking it
    return 0;
}

// ---- once ------------------------------------------------------------------

typedef INIT_ONCE pthread_once_t;
#define PTHREAD_ONCE_INIT INIT_ONCE_STATIC_INIT

static inline BOOL CALLBACK rs_once_trampoline(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once; (void)ctx;
    ((void (*)(void))param)();
    return TRUE;
}

static inline int pthread_once(pthread_once_t *once, void (*fn)(void)) {
    if (!once || !fn) return EINVAL;
    return InitOnceExecuteOnce(once, rs_once_trampoline, (PVOID)fn, NULL) ? 0 : EINVAL;
}

// ---- thread-local storage --------------------------------------------------
// FLS, not TLS: only FlsAlloc runs a destructor when a thread exits, and that
// destructor is the whole point here — net.c parks a per-thread CURL handle in a
// key so each worker reuses its connections, and it has to be cleaned up when
// the worker dies or every finished thread leaks a handle and its sockets.
//
// The destructor is stored beside the value rather than in a per-key table, so
// one shared callback serves every key and the calling convention is ours.

// The key is a heap descriptor rather than a bare FLS index, because the
// destructor has to be reachable from pthread_setspecific and a `static` table
// in a header would give every translation unit its own private copy of it —
// key_create in one file, setspecific in another, and the destructor silently
// never runs. One allocation per key, and there is one key in the whole tree.

typedef struct rs_tls_key {
    DWORD fls;
    void (*dtor)(void *);
} *pthread_key_t;

typedef struct {
    void *value;
    void (*dtor)(void *);
} rs_tls_slot;

static inline void WINAPI rs_tls_cleanup(PVOID raw) {
    rs_tls_slot *slot = (rs_tls_slot *)raw;
    if (!slot) return;
    if (slot->dtor && slot->value) slot->dtor(slot->value);
    free(slot);
}

static inline int pthread_key_create(pthread_key_t *key, void (*dtor)(void *)) {
    struct rs_tls_key *k;
    if (!key) return EINVAL;
    k = (struct rs_tls_key *)malloc(sizeof(*k));
    if (!k) return EAGAIN;
    k->fls = FlsAlloc(rs_tls_cleanup);
    if (k->fls == FLS_OUT_OF_INDEXES) { free(k); return EAGAIN; }
    k->dtor = dtor;
    *key = k;
    return 0;
}

static inline int pthread_key_delete(pthread_key_t key) {
    BOOL ok;
    if (!key) return EINVAL;
    ok = FlsFree(key->fls);
    free(key);
    return ok ? 0 : EINVAL;
}

static inline void *pthread_getspecific(pthread_key_t key) {
    rs_tls_slot *slot;
    if (!key) return NULL;
    slot = (rs_tls_slot *)FlsGetValue(key->fls);
    return slot ? slot->value : NULL;
}

static inline int pthread_setspecific(pthread_key_t key, const void *value) {
    rs_tls_slot *slot;
    if (!key) return EINVAL;
    slot = (rs_tls_slot *)FlsGetValue(key->fls);
    if (!slot) {
        slot = (rs_tls_slot *)malloc(sizeof(*slot));
        if (!slot) return ENOMEM;
        slot->dtor = key->dtor;
        slot->value = NULL;
        if (!FlsSetValue(key->fls, slot)) { free(slot); return EINVAL; }
    }
    slot->value = (void *)value;
    return 0;
}

#endif  // _WIN32
#endif  // RS_THREAD_H
