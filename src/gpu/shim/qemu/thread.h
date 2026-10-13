/* Win32 stand-in for QEMU's "qemu/thread.h" (see src/gpu/PROVENANCE.md).
 *
 * QemuMutex is non-recursive in QEMU and xemu relies on that shape (it
 * drops pfifo.lock before taking pgraph.lock and re-takes it afterwards), so
 * an SRW lock is the faithful match; condition variables sleep on it.
 * QemuEvent is a manual-reset event, exactly QEMU's semantics: set wakes all
 * waiters and stays set until reset. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

typedef struct QemuMutex { SRWLOCK lock; } QemuMutex;
typedef struct QemuCond { CONDITION_VARIABLE cv; } QemuCond;
typedef struct QemuEvent { HANDLE ev; } QemuEvent;
typedef struct QemuThread { HANDLE h; } QemuThread;

#define QEMU_THREAD_JOINABLE 0
#define QEMU_THREAD_DETACHED 1

static inline void qemu_mutex_init(QemuMutex *m) { InitializeSRWLock(&m->lock); }
static inline void qemu_mutex_destroy(QemuMutex *m) { (void)m; }
static inline void qemu_mutex_lock(QemuMutex *m) { AcquireSRWLockExclusive(&m->lock); }
static inline void qemu_mutex_unlock(QemuMutex *m) { ReleaseSRWLockExclusive(&m->lock); }
static inline bool qemu_mutex_trylock(QemuMutex *m)
{
    return TryAcquireSRWLockExclusive(&m->lock) ? false : true; /* 0 = got it */
}

static inline void qemu_cond_init(QemuCond *c) { InitializeConditionVariable(&c->cv); }
static inline void qemu_cond_destroy(QemuCond *c) { (void)c; }
static inline void qemu_cond_signal(QemuCond *c) { WakeConditionVariable(&c->cv); }
static inline void qemu_cond_broadcast(QemuCond *c) { WakeAllConditionVariable(&c->cv); }
static inline void qemu_cond_wait(QemuCond *c, QemuMutex *m)
{
    SleepConditionVariableSRW(&c->cv, &m->lock, INFINITE, 0);
}
static inline bool qemu_cond_timedwait(QemuCond *c, QemuMutex *m, int ms)
{
    return SleepConditionVariableSRW(&c->cv, &m->lock, (DWORD)ms, 0) != 0;
}

static inline void qemu_event_init(QemuEvent *e, bool init)
{
    e->ev = CreateEventA(NULL, TRUE, init ? TRUE : FALSE, NULL);
}
static inline void qemu_event_destroy(QemuEvent *e) { CloseHandle(e->ev); }
static inline void qemu_event_set(QemuEvent *e) { SetEvent(e->ev); }
static inline void qemu_event_reset(QemuEvent *e) { ResetEvent(e->ev); }
static inline void qemu_event_wait(QemuEvent *e) { WaitForSingleObject(e->ev, INFINITE); }

typedef struct {
    void *(*fn)(void *);
    void *arg;
} QemuThreadStart;

static DWORD WINAPI qemu_thread_trampoline(LPVOID p)
{
    QemuThreadStart s = *(QemuThreadStart *)p;
    free(p);
    s.fn(s.arg);
    return 0;
}

static inline void qemu_thread_create(QemuThread *t, const char *name,
                                      void *(*fn)(void *), void *arg, int mode)
{
    QemuThreadStart *s = (QemuThreadStart *)malloc(sizeof(*s));
    (void)name; (void)mode;
    s->fn = fn;
    s->arg = arg;
    t->h = CreateThread(NULL, 0, qemu_thread_trampoline, s, 0, NULL);
}

static inline void *qemu_thread_join(QemuThread *t)
{
    WaitForSingleObject(t->h, INFINITE);
    CloseHandle(t->h);
    return NULL;
}

/* The Big QEMU Lock: in QEMU every MMIO handler and timer runs under it and
 * PGRAPH takes it around interrupt updates. nv2a_recomp.c keeps the same
 * discipline with a recursive critical section. */
void bql_lock(void);
void bql_unlock(void);
