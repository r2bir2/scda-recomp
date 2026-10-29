/*
 * win32_compat_ext.c - further Win32 primitives on POSIX for the SCDA ARM
 * port (docs Update 677): vectored exception handlers on top of signals,
 * SRW locks, one-time init, waitable timers, child processes, thread
 * affinity and the MSVC "secure" CRT functions the runtime uses.
 *
 * Linux/POSIX only.
 */
#if !defined(_WIN32)
#define _GNU_SOURCE
#include "win32_compat.h"
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <ucontext.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <limits.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/resource.h>

extern char **environ;
#ifndef STILL_ACTIVE
#define STILL_ACTIVE 259u
#endif

/* ===================================================================== */
/* Vectored exception handlers                                           */
/* ===================================================================== */

#define VEH_MAX 32
static PVECTORED_EXCEPTION_HANDLER s_veh[VEH_MAX];
static int s_veh_count;
static pthread_mutex_t s_veh_lock = PTHREAD_MUTEX_INITIALIZER;
static LPTOP_LEVEL_EXCEPTION_FILTER s_unhandled_filter;
static int s_signals_installed;

#if defined(__aarch64__)
/* the data-abort syndrome: bit 6 (WnR) tells a write from a read */
static int fault_is_write(const ucontext_t *uc)
{
    const uint8_t *p = (const uint8_t *)uc->uc_mcontext.__reserved;
    const uint8_t *end = p + sizeof(uc->uc_mcontext.__reserved);
    while (p + 8 <= end) {
        uint32_t magic = *(const uint32_t *)p;
        uint32_t size = *(const uint32_t *)(p + 4);
        if (magic == 0 || size < 8)
            break;
        if (magic == 0x45535201u /* ESR_MAGIC */ && size >= 16) {
            uint64_t esr = *(const uint64_t *)(p + 8);
            return (int)((esr >> 6) & 1u);
        }
        p += size;
    }
    return 0;
}

static void context_from_uc(CONTEXT *c, const ucontext_t *uc)
{
    for (int i = 0; i < 31; i++)
        c->X[i] = uc->uc_mcontext.regs[i];
    c->Sp = uc->uc_mcontext.sp;
    c->Pc = uc->uc_mcontext.pc;
    c->Pstate = uc->uc_mcontext.pstate;
}

static void context_to_uc(const CONTEXT *c, ucontext_t *uc)
{
    for (int i = 0; i < 31; i++)
        uc->uc_mcontext.regs[i] = c->X[i];
    uc->uc_mcontext.sp = c->Sp;
    uc->uc_mcontext.pc = c->Pc;
    uc->uc_mcontext.pstate = c->Pstate;
}
#elif defined(__x86_64__)
static int fault_is_write(const ucontext_t *uc)
{
    return (int)((uc->uc_mcontext.gregs[REG_ERR] >> 1) & 1);
}

static void context_from_uc(CONTEXT *c, const ucontext_t *uc)
{
    const greg_t *g = uc->uc_mcontext.gregs;
    c->Rax = g[REG_RAX]; c->Rcx = g[REG_RCX]; c->Rdx = g[REG_RDX];
    c->Rbx = g[REG_RBX]; c->Rsp = g[REG_RSP]; c->Rbp = g[REG_RBP];
    c->Rsi = g[REG_RSI]; c->Rdi = g[REG_RDI]; c->R8 = g[REG_R8];
    c->R9 = g[REG_R9]; c->R10 = g[REG_R10]; c->R11 = g[REG_R11];
    c->R12 = g[REG_R12]; c->R13 = g[REG_R13]; c->R14 = g[REG_R14];
    c->R15 = g[REG_R15]; c->Rip = g[REG_RIP]; c->EFlags = (uint32_t)g[REG_EFL];
}

static void context_to_uc(const CONTEXT *c, ucontext_t *uc)
{
    greg_t *g = uc->uc_mcontext.gregs;
    g[REG_RAX] = c->Rax; g[REG_RCX] = c->Rcx; g[REG_RDX] = c->Rdx;
    g[REG_RBX] = c->Rbx; g[REG_RSP] = c->Rsp; g[REG_RBP] = c->Rbp;
    g[REG_RSI] = c->Rsi; g[REG_RDI] = c->Rdi; g[REG_R8] = c->R8;
    g[REG_R9] = c->R9; g[REG_R10] = c->R10; g[REG_R11] = c->R11;
    g[REG_R12] = c->R12; g[REG_R13] = c->R13; g[REG_R14] = c->R14;
    g[REG_R15] = c->R15; g[REG_RIP] = c->Rip; g[REG_EFL] = c->EFlags;
}
#else
#error "win32_compat_ext.c: unsupported host architecture"
#endif

static void veh_signal(int sig, siginfo_t *si, void *uc_void)
{
    ucontext_t *uc = (ucontext_t *)uc_void;
    EXCEPTION_RECORD rec;
    CONTEXT ctx;
    EXCEPTION_POINTERS ep = { &rec, &ctx };
    PVECTORED_EXCEPTION_HANDLER list[VEH_MAX];
    int n, saved_errno = errno;

    memset(&rec, 0, sizeof(rec));
    memset(&ctx, 0, sizeof(ctx));
    context_from_uc(&ctx, uc);
    ctx.ContextFlags = CONTEXT_FULL;
    ctx.HostContext = uc;
    switch (sig) {
    case SIGSEGV:
    case SIGBUS:
        rec.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
        rec.NumberParameters = 2;
        rec.ExceptionInformation[0] = (ULONG_PTR)fault_is_write(uc);
        rec.ExceptionInformation[1] = (ULONG_PTR)si->si_addr;
        break;
    case SIGILL:
        rec.ExceptionCode = EXCEPTION_ILLEGAL_INSTRUCTION;
        break;
    case SIGFPE:
        rec.ExceptionCode = EXCEPTION_INT_DIVIDE_BY_ZERO;
        break;
    default:
        rec.ExceptionCode = 0xE0000000u | (DWORD)sig;
        break;
    }
    rec.ExceptionAddress = (PVOID)(uintptr_t)XBOX_CONTEXT_PC(&ctx);

    /* a snapshot: handlers do not take the lock (they may fault themselves) */
    n = s_veh_count;
    memcpy(list, s_veh, sizeof(list));
    for (int i = 0; i < n; i++) {
        if (list[i] && list[i](&ep) == EXCEPTION_CONTINUE_EXECUTION) {
            context_to_uc(&ctx, uc);
            errno = saved_errno;
            return;
        }
    }
    if (s_unhandled_filter &&
        s_unhandled_filter(&ep) == EXCEPTION_CONTINUE_EXECUTION) {
        context_to_uc(&ctx, uc);
        errno = saved_errno;
        return;
    }
    /* nobody took it: default action, the fault repeats and terminates */
    signal(sig, SIG_DFL);
    errno = saved_errno;
}

static void veh_install_signals(void)
{
    struct sigaction sa;
    if (s_signals_installed)
        return;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = veh_signal;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    s_signals_installed = 1;
}

PVOID AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER handler)
{
    if (!handler)
        return NULL;
    pthread_mutex_lock(&s_veh_lock);
    if (s_veh_count < VEH_MAX) {
        if (first) {
            memmove(&s_veh[1], &s_veh[0], sizeof(s_veh[0]) * s_veh_count);
            s_veh[0] = handler;
        } else {
            s_veh[s_veh_count] = handler;
        }
        s_veh_count++;
    }
    veh_install_signals();
    pthread_mutex_unlock(&s_veh_lock);
    return (PVOID)handler;
}

ULONG RemoveVectoredExceptionHandler(PVOID h)
{
    pthread_mutex_lock(&s_veh_lock);
    for (int i = 0; i < s_veh_count; i++) {
        if ((PVOID)s_veh[i] == h) {
            memmove(&s_veh[i], &s_veh[i + 1],
                    sizeof(s_veh[0]) * (s_veh_count - i - 1));
            s_veh_count--;
            break;
        }
    }
    pthread_mutex_unlock(&s_veh_lock);
    return 1;
}

LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER f)
{
    LPTOP_LEVEL_EXCEPTION_FILTER old = s_unhandled_filter;
    s_unhandled_filter = f;
    pthread_mutex_lock(&s_veh_lock);
    veh_install_signals();
    pthread_mutex_unlock(&s_veh_lock);
    return old;
}

/* ===================================================================== */
/* SRW locks, one-time init                                              */
/* ===================================================================== */

static pthread_rwlock_t *srw_get(PSRWLOCK l)
{
    pthread_rwlock_t *rw = (pthread_rwlock_t *)__atomic_load_n(&l->Ptr, __ATOMIC_ACQUIRE);
    if (!rw) {
        pthread_rwlock_t *fresh = (pthread_rwlock_t *)malloc(sizeof(*fresh));
        void *expected = NULL;
        pthread_rwlock_init(fresh, NULL);
        if (__atomic_compare_exchange_n(&l->Ptr, &expected, fresh, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            rw = fresh;
        } else {
            pthread_rwlock_destroy(fresh);
            free(fresh);
            rw = (pthread_rwlock_t *)expected;
        }
    }
    return rw;
}

VOID InitializeSRWLock(PSRWLOCK l) { l->Ptr = NULL; }
VOID AcquireSRWLockExclusive(PSRWLOCK l) { pthread_rwlock_wrlock(srw_get(l)); }
VOID AcquireSRWLockShared(PSRWLOCK l) { pthread_rwlock_rdlock(srw_get(l)); }
VOID ReleaseSRWLockExclusive(PSRWLOCK l) { pthread_rwlock_unlock(srw_get(l)); }
VOID ReleaseSRWLockShared(PSRWLOCK l) { pthread_rwlock_unlock(srw_get(l)); }
BOOLEAN TryAcquireSRWLockExclusive(PSRWLOCK l) { return pthread_rwlock_trywrlock(srw_get(l)) == 0; }
BOOLEAN TryAcquireSRWLockShared(PSRWLOCK l) { return pthread_rwlock_tryrdlock(srw_get(l)) == 0; }

BOOL SleepConditionVariableSRW(PCONDITION_VARIABLE cv, PSRWLOCK l, DWORD ms, ULONG flags)
{
    /* rwlocks cannot wait on a condition: release, sleep briefly, reacquire */
    (void)cv;
    if (flags & 1u) ReleaseSRWLockShared(l); else ReleaseSRWLockExclusive(l);
    Sleep(ms == INFINITE ? 1 : (ms < 1 ? 0 : 1));
    if (flags & 1u) AcquireSRWLockShared(l); else AcquireSRWLockExclusive(l);
    return TRUE;
}

BOOL InitOnceExecuteOnce(PINIT_ONCE once, PINIT_ONCE_FN fn, PVOID param, LPVOID *context)
{
    /* 0 untouched, 1 running, 2 done */
    LONG expected = 0;
    if (__atomic_compare_exchange_n(&once->State, &expected, 1, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        PVOID ctx = NULL;
        BOOL ok = fn(once, param, &ctx);
        once->Context = ctx;
        __atomic_store_n(&once->State, ok ? 2 : 0, __ATOMIC_RELEASE);
        if (context) *context = ctx;
        return ok;
    }
    while (__atomic_load_n(&once->State, __ATOMIC_ACQUIRE) == 1)
        sched_yield();
    if (context) *context = once->Context;
    return __atomic_load_n(&once->State, __ATOMIC_ACQUIRE) == 2;
}

/* ===================================================================== */
/* Threads                                                               */
/* ===================================================================== */

HANDLE OpenThread(DWORD access, BOOL inherit, DWORD tid)
{
    (void)access; (void)inherit; (void)tid;
    return NULL;
}

BOOL GetThreadContext(HANDLE thread, PCONTEXT ctx)
{
    (void)thread; (void)ctx;
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

BOOL SetThreadContext(HANDLE thread, const CONTEXT *ctx)
{
    (void)thread; (void)ctx;
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

DWORD_PTR SetThreadAffinityMask(HANDLE thread, DWORD_PTR mask)
{
    /* only the calling thread (the runtime pins each thread itself) */
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int i = 0; i < (int)(8 * sizeof(mask)) && i < CPU_SETSIZE; i++)
        if (mask & ((DWORD_PTR)1 << i))
            CPU_SET(i, &set);
    pthread_t t;
    if (thread == GetCurrentThread() || thread == (HANDLE)(LONG_PTR)-2)
        t = pthread_self();
    else if (!w32_thread_pthread(thread, &t))
        return 0;
    if (pthread_setaffinity_np(t, sizeof(set), &set) != 0)
        return 0;
    return mask;
}

BOOL SetThreadStackGuarantee(PULONG size) { (void)size; return TRUE; }

HRESULT SetThreadDescription(HANDLE thread, LPCWSTR name)
{
    char buf[16];
    int i = 0;
    if (thread != GetCurrentThread() && thread != (HANDLE)(LONG_PTR)-2)
        return S_OK;
    for (; name && name[i] && i < 15; i++)
        buf[i] = (char)(name[i] < 128 ? name[i] : '?');
    buf[i] = 0;
    pthread_setname_np(pthread_self(), buf);
    return S_OK;
}

BOOL GetProcessAffinityMask(HANDLE proc, PDWORD_PTR proc_mask, PDWORD_PTR sys_mask)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    DWORD_PTR m = (n >= (long)(8 * sizeof(DWORD_PTR))) ? ~(DWORD_PTR)0 :
                  (((DWORD_PTR)1 << n) - 1);
    (void)proc;
    if (proc_mask) *proc_mask = m;
    if (sys_mask) *sys_mask = m;
    return TRUE;
}

BOOL SetPriorityClass(HANDLE proc, DWORD cls) { (void)proc; (void)cls; return TRUE; }

/* ===================================================================== */
/* Waitable timers                                                       */
/* ===================================================================== */

typedef struct w32_wtimer {
    HANDLE event;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int armed, quit, generation;
    struct timespec due;
    LONG period_ms;
    pthread_t thread;
} w32_wtimer;

#define WTIMER_MAX 64
static w32_wtimer *s_wtimers[WTIMER_MAX];
static pthread_mutex_t s_wtimer_lock = PTHREAD_MUTEX_INITIALIZER;

static w32_wtimer *wtimer_of(HANDLE h)
{
    w32_wtimer *t = NULL;
    pthread_mutex_lock(&s_wtimer_lock);
    for (int i = 0; i < WTIMER_MAX; i++)
        if (s_wtimers[i] && s_wtimers[i]->event == h)
            t = s_wtimers[i];
    pthread_mutex_unlock(&s_wtimer_lock);
    return t;
}

static void ts_add_ms(struct timespec *ts, long ms)
{
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}

static void *wtimer_thread(void *arg)
{
    w32_wtimer *t = (w32_wtimer *)arg;
    pthread_mutex_lock(&t->lock);
    for (;;) {
        while (!t->armed && !t->quit)
            pthread_cond_wait(&t->cond, &t->lock);
        if (t->quit)
            break;
        int gen = t->generation;
        int rc = pthread_cond_timedwait(&t->cond, &t->lock, &t->due);
        if (t->quit)
            break;
        if (rc == ETIMEDOUT && t->armed && gen == t->generation) {
            SetEvent(t->event);
            if (t->period_ms > 0)
                ts_add_ms(&t->due, t->period_ms);
            else
                t->armed = 0;
        }
    }
    pthread_mutex_unlock(&t->lock);
    return NULL;
}

static HANDLE wtimer_create(BOOL manual)
{
    w32_wtimer *t = (w32_wtimer *)calloc(1, sizeof(*t));
    pthread_condattr_t ca;
    t->event = CreateEventA(NULL, manual, FALSE, NULL);
    pthread_mutex_init(&t->lock, NULL);
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&t->cond, &ca);
    pthread_create(&t->thread, NULL, wtimer_thread, t);
    pthread_mutex_lock(&s_wtimer_lock);
    for (int i = 0; i < WTIMER_MAX; i++) {
        if (!s_wtimers[i]) {
            s_wtimers[i] = t;
            break;
        }
    }
    pthread_mutex_unlock(&s_wtimer_lock);
    return t->event;
}

HANDLE CreateWaitableTimerA(LPSECURITY_ATTRIBUTES sa, BOOL manual, LPCSTR name)
{ (void)sa; (void)name; return wtimer_create(manual); }
HANDLE CreateWaitableTimerW(LPSECURITY_ATTRIBUTES sa, BOOL manual, LPCWSTR name)
{ (void)sa; (void)name; return wtimer_create(manual); }
HANDLE CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES sa, LPCWSTR name, DWORD flags, DWORD access)
{ (void)sa; (void)name; (void)access; return wtimer_create((flags & CREATE_WAITABLE_TIMER_MANUAL_RESET) != 0); }

BOOL SetWaitableTimer(HANDLE h, const LARGE_INTEGER *due, LONG period_ms,
                      PTIMERAPCROUTINE apc, LPVOID arg, BOOL resume)
{
    w32_wtimer *t = wtimer_of(h);
    (void)apc; (void)arg; (void)resume;
    if (!t || !due)
        return FALSE;
    pthread_mutex_lock(&t->lock);
    clock_gettime(CLOCK_MONOTONIC, &t->due);
    if (due->QuadPart < 0) {
        long long ns = -due->QuadPart * 100;   /* relative, 100 ns units */
        t->due.tv_sec += ns / 1000000000LL;
        t->due.tv_nsec += ns % 1000000000LL;
        if (t->due.tv_nsec >= 1000000000L) { t->due.tv_sec++; t->due.tv_nsec -= 1000000000L; }
    }   /* absolute times: due now (only relative ones are used) */
    t->period_ms = period_ms;
    t->armed = 1;
    t->generation++;
    ResetEvent(t->event);
    pthread_cond_broadcast(&t->cond);
    pthread_mutex_unlock(&t->lock);
    return TRUE;
}

BOOL CancelWaitableTimer(HANDLE h)
{
    w32_wtimer *t = wtimer_of(h);
    if (!t)
        return FALSE;
    pthread_mutex_lock(&t->lock);
    t->armed = 0;
    t->generation++;
    pthread_cond_broadcast(&t->cond);
    pthread_mutex_unlock(&t->lock);
    return TRUE;
}

/* ===================================================================== */
/* Processes                                                             */
/* ===================================================================== */

typedef struct w32_proc {
    HANDLE done;            /* manual-reset event, set when the child exits */
    pid_t pid;
    int exit_code;
} w32_proc;

#define PROC_MAX 16
static w32_proc *s_procs[PROC_MAX];

static void *proc_reaper(void *arg)
{
    w32_proc *p = (w32_proc *)arg;
    int status = 0;
    waitpid(p->pid, &status, 0);
    p->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    SetEvent(p->done);
    return NULL;
}

/* splits a Windows command line into argv (quotes group, no escapes) */
static char **split_cmdline(const char *cmd, int *argc_out)
{
    char **argv = (char **)calloc(64, sizeof(char *));
    int argc = 0;
    const char *p = cmd;
    while (p && *p && argc < 63) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char buf[4096];
        size_t n = 0;
        int q = 0;
        while (*p && (q || (*p != ' ' && *p != '\t'))) {
            if (*p == '"') { q = !q; p++; continue; }
            if (n + 1 < sizeof(buf)) buf[n++] = *p;
            p++;
        }
        buf[n] = 0;
        argv[argc++] = strdup(buf);
    }
    *argc_out = argc;
    return argv;
}

static void path_to_posix(char *s)
{
    /* "Z:\home\..." (Wine-style) or "C:\..." -> "/..." with forward slashes */
    for (char *c = s; *c; c++)
        if (*c == '\\') *c = '/';
    if (((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')) && s[1] == ':')
        memmove(s, s + 2, strlen(s + 2) + 1);
}

BOOL CreateProcessA(LPCSTR app, LPSTR cmdline, LPSECURITY_ATTRIBUTES pa,
                    LPSECURITY_ATTRIBUTES ta, BOOL inherit, DWORD flags, LPVOID env,
                    LPCSTR cwd, LPSTARTUPINFOA si, LPPROCESS_INFORMATION pi)
{
    int argc = 0;
    char **argv = split_cmdline(cmdline ? cmdline : (app ? app : ""), &argc);
    char exe[4096];
    pid_t pid;
    int rc;
    (void)pa; (void)ta; (void)inherit; (void)flags; (void)env; (void)si;
    if (argc == 0 && app)
        argv[argc++] = strdup(app);
    snprintf(exe, sizeof(exe), "%s", app ? app : argv[0]);
    path_to_posix(exe);
    for (int i = 0; i < argc; i++)
        path_to_posix(argv[i]);
    if (cwd) {
        char d[4096];
        snprintf(d, sizeof(d), "%s", cwd);
        path_to_posix(d);
        if (chdir(d) != 0) { /* keep the current directory */ }
    }
    rc = posix_spawn(&pid, exe, NULL, NULL, argv, environ);
    for (int i = 0; i < argc; i++) free(argv[i]);
    free(argv);
    if (rc != 0) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return FALSE;
    }
    w32_proc *p = (w32_proc *)calloc(1, sizeof(*p));
    pthread_t th;
    p->pid = pid;
    p->done = CreateEventA(NULL, TRUE, FALSE, NULL);
    for (int i = 0; i < PROC_MAX; i++)
        if (!s_procs[i]) { s_procs[i] = p; break; }
    pthread_create(&th, NULL, proc_reaper, p);
    pthread_detach(th);
    if (pi) {
        pi->hProcess = p->done;
        pi->hThread = NULL;
        pi->dwProcessId = (DWORD)pid;
        pi->dwThreadId = 0;
    }
    return TRUE;
}

static char *wide_to_utf8(LPCWSTR w)
{
    size_t n = 0;
    char *s;
    if (!w) return NULL;
    while (w[n]) n++;
    s = (char *)malloc(n * 3 + 1);
    WideCharToMultiByte(65001 /* CP_UTF8 */, 0, w, -1, s, (int)(n * 3 + 1), NULL, NULL);
    return s;
}

BOOL CreateProcessW(LPCWSTR app, LPWSTR cmdline, LPSECURITY_ATTRIBUTES pa,
                    LPSECURITY_ATTRIBUTES ta, BOOL inherit, DWORD flags, LPVOID env,
                    LPCWSTR cwd, LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi)
{
    char *a = wide_to_utf8(app), *c = wide_to_utf8(cmdline), *d = wide_to_utf8(cwd);
    BOOL ok;
    (void)si;
    ok = CreateProcessA(a, c, pa, ta, inherit, flags, env, d, NULL, pi);
    free(a); free(c); free(d);
    return ok;
}

HANDLE OpenProcess(DWORD access, BOOL inherit, DWORD pid)
{
    (void)access; (void)inherit; (void)pid;
    return NULL;
}

BOOL GetExitCodeProcess(HANDLE proc, LPDWORD code)
{
    for (int i = 0; i < PROC_MAX; i++) {
        w32_proc *p = s_procs[i];
        if (p && p->done == proc) {
            if (code)
                *code = WaitForSingleObject(p->done, 0) == WAIT_OBJECT_0 ?
                            (DWORD)p->exit_code : STILL_ACTIVE;
            return TRUE;
        }
    }
    return FALSE;
}

static char s_cmdline[4096];
static uint16_t s_cmdline_w[4096];

LPSTR GetCommandLineA(void)
{
    if (!s_cmdline[0]) {
        FILE *f = fopen("/proc/self/cmdline", "rb");
        size_t n = f ? fread(s_cmdline, 1, sizeof(s_cmdline) - 1, f) : 0;
        if (f) fclose(f);
        for (size_t i = 0; i + 1 < n; i++)
            if (!s_cmdline[i]) s_cmdline[i] = ' ';
        s_cmdline[n] = 0;
    }
    return s_cmdline;
}

LPWSTR GetCommandLineW(void)
{
    const char *a = GetCommandLineA();
    size_t i = 0;
    for (; a[i] && i + 1 < sizeof(s_cmdline_w) / 2; i++)
        s_cmdline_w[i] = (uint8_t)a[i];
    s_cmdline_w[i] = 0;
    return (LPWSTR)s_cmdline_w;
}

/* ===================================================================== */
/* MSVC secure CRT                                                       */
/* ===================================================================== */

errno_t strcpy_s(char *dst, size_t size, const char *src)
{
    size_t n;
    if (!dst || !size) return EINVAL;
    n = src ? strlen(src) : 0;
    if (n >= size) { dst[0] = 0; return ERANGE; }
    memcpy(dst, src, n + 1);
    return 0;
}

errno_t strncpy_s(char *dst, size_t size, const char *src, size_t count)
{
    size_t n;
    if (!dst || !size) return EINVAL;
    n = src ? strnlen(src, count == _TRUNCATE ? size - 1 : count) : 0;
    if (n >= size) n = size - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
    return 0;
}

errno_t strcat_s(char *dst, size_t size, const char *src)
{
    size_t d, n;
    if (!dst || !size) return EINVAL;
    d = strnlen(dst, size);
    n = src ? strlen(src) : 0;
    if (d + n >= size) return ERANGE;
    memcpy(dst + d, src, n + 1);
    return 0;
}

errno_t strncat_s(char *dst, size_t size, const char *src, size_t count)
{
    size_t d, n;
    if (!dst || !size) return EINVAL;
    d = strnlen(dst, size);
    n = src ? strnlen(src, count) : 0;
    if (d + n >= size) n = size - d - 1;
    memcpy(dst + d, src, n);
    dst[d + n] = 0;
    return 0;
}

int vsprintf_s(char *dst, size_t size, const char *fmt, va_list ap)
{
    return w32_vsnprintf(dst, size, fmt, ap);
}

int sprintf_s(char *dst, size_t size, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = w32_vsnprintf(dst, size, fmt, ap);
    va_end(ap);
    return r;
}

int _vsnprintf_s(char *dst, size_t size, size_t count, const char *fmt, va_list ap)
{
    size_t lim = (count == _TRUNCATE || count + 1 > size) ? size : count + 1;
    int r = w32_vsnprintf(dst, lim, fmt, ap);
    return (r >= 0 && (size_t)r < lim) ? r : -1;
}

int _snprintf_s(char *dst, size_t size, size_t count, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = _vsnprintf_s(dst, size, count, fmt, ap);
    va_end(ap);
    return r;
}

static void mode_fix(const char *path, char *out)
{
    w32_host_path(path, out, 4096);
}

errno_t fopen_s(FILE **fp, const char *path, const char *mode)
{
    char p[4096];
    if (!fp) return EINVAL;
    mode_fix(path, p);
    *fp = fopen(p, mode);
    return *fp ? 0 : errno;
}

FILE *_fsopen(const char *path, const char *mode, int shflag)
{
    char p[4096];
    (void)shflag;
    mode_fix(path, p);
    return fopen(p, mode);
}

errno_t _putenv_s(const char *name, const char *value)
{
    if (value && value[0]) return setenv(name, value, 1) ? errno : 0;
    return unsetenv(name) ? errno : 0;
}

errno_t _dupenv_s(char **buf, size_t *len, const char *name)
{
    const char *v = getenv(name);
    if (!buf) return EINVAL;
    *buf = v ? strdup(v) : NULL;
    if (len) *len = v ? strlen(v) + 1 : 0;
    return 0;
}

errno_t getenv_s(size_t *len, char *buf, size_t size, const char *name)
{
    const char *v = getenv(name);
    size_t n = v ? strlen(v) + 1 : 0;
    if (len) *len = n;
    if (!v) { if (buf && size) buf[0] = 0; return 0; }
    if (buf && size) {
        if (n > size) { buf[0] = 0; return ERANGE; }
        memcpy(buf, v, n);
    }
    return 0;
}

errno_t memcpy_s(void *dst, size_t dsize, const void *src, size_t count)
{
    if (count > dsize) return ERANGE;
    memcpy(dst, src, count);
    return 0;
}

/* ===================================================================== */
/* WaitOnAddress, modules, environment, debugging                        */
/* ===================================================================== */

/* futexes work on aligned 32-bit words: a narrower or wider address waits
 * on the word holding its first byte, which only adds spurious wake-ups
 * (WaitOnAddress allows those) */
static int *futex_word(volatile void *addr)
{
    return (int *)((uintptr_t)addr & ~(uintptr_t)3);
}

BOOL WaitOnAddress(volatile VOID *addr, PVOID compare, SIZE_T size, DWORD ms)
{
    int *word = futex_word(addr);
    int seen = __atomic_load_n(word, __ATOMIC_SEQ_CST);
    if (memcmp((const void *)addr, compare, size) != 0)
        return TRUE;
    struct timespec ts, *tsp = NULL;
    if (ms != INFINITE) {
        ts.tv_sec = ms / 1000;
        ts.tv_nsec = (long)(ms % 1000) * 1000000L;
        tsp = &ts;
    }
    if (syscall(SYS_futex, word, FUTEX_WAIT_PRIVATE, seen, tsp, NULL, 0) != 0 &&
        errno == ETIMEDOUT) {
        SetLastError(ERROR_TIMEOUT);
        return FALSE;
    }
    return TRUE;
}

VOID WakeByAddressSingle(PVOID addr)
{
    /* every waiter of a shared word may be someone else's: wake them all */
    syscall(SYS_futex, futex_word(addr), FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0);
}

VOID WakeByAddressAll(PVOID addr)
{
    syscall(SYS_futex, futex_word(addr), FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0);
}

/* The executable is the only module; GetModuleHandle(NULL) is its load base
 * (diagnostics print addresses relative to it). Named DLLs do not exist. */
HMODULE GetModuleHandleA(LPCSTR name)
{
    Dl_info info;
    if (name) return NULL;
    if (dladdr((void *)&GetModuleHandleA, &info) && info.dli_fbase)
        return (HMODULE)info.dli_fbase;
    return NULL;
}

HMODULE GetModuleHandleW(LPCWSTR name)
{
    return name ? NULL : GetModuleHandleA(NULL);
}

/* a module handle is either GetModuleHandle(NULL)'s load base or a
 * dlopen handle; dlopen handles are tagged in the low bit */
#define DL_TAG 1u

void *GetProcAddress(HMODULE mod, LPCSTR name)
{
    if (!mod || !name) return NULL;
    if ((uintptr_t)mod & DL_TAG)
        return dlsym((void *)((uintptr_t)mod & ~(uintptr_t)DL_TAG), name);
    return dlsym(RTLD_DEFAULT, name);
}

HMODULE LoadLibraryA(LPCSTR path)
{
    char host[4096];
    if (!path) return NULL;
    void *h = dlopen(w32_host_path(path, host, sizeof(host)), RTLD_NOW | RTLD_LOCAL);
    if (!h) { SetLastError(ERROR_FILE_NOT_FOUND); return NULL; }
    return (HMODULE)((uintptr_t)h | DL_TAG);
}

HMODULE LoadLibraryExW(LPCWSTR path, HANDLE file, DWORD flags)
{
    char p[4096];
    (void)file; (void)flags;
    if (!path || WideCharToMultiByte(CP_UTF8, 0, path, -1, p, sizeof(p), NULL, NULL) <= 0)
        return NULL;
    return LoadLibraryA(p);
}

BOOL FreeLibrary(HMODULE mod)
{
    if (!((uintptr_t)mod & DL_TAG)) return FALSE;
    return dlclose((void *)((uintptr_t)mod & ~(uintptr_t)DL_TAG)) == 0;
}

BOOL SetEnvironmentVariableA(LPCSTR name, LPCSTR value)
{
    if (!name) return FALSE;
    return (value ? setenv(name, value, 1) : unsetenv(name)) == 0;
}

VOID GetSystemTimePreciseAsFileTime(LPFILETIME ft)
{
    GetSystemTimeAsFileTime(ft);   /* already clock_gettime-precise */
}

BOOL IsDebuggerPresent(void)
{
    char line[256];
    BOOL traced = FALSE;
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return FALSE;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "TracerPid:", 10)) {
            traced = atoi(line + 10) != 0;
            break;
        }
    }
    fclose(f);
    return traced;
}

VOID DebugBreak(void)
{
    raise(SIGTRAP);
}

USHORT RtlCaptureStackBackTrace(ULONG skip, ULONG count, PVOID *frames, PULONG hash)
{
    void *buf[128];
    int n = backtrace(buf, 128);
    USHORT out = 0;
    ULONG h = 0;
    for (int i = (int)skip + 1; i < n && out < count; i++, out++) {
        frames[out] = buf[i];
        h += (ULONG)(uintptr_t)buf[i];
    }
    if (hash) *hash = h;
    return out;
}

/* no pointer position outside the SDL window thread */
BOOL GetCursorPos(LPPOINT p)
{
    if (p) p->x = p->y = 0;
    return FALSE;
}

BOOL ScreenToClient(HWND hwnd, LPPOINT p)
{
    (void)hwnd; (void)p;
    return FALSE;
}

BOOL ShowWindow(HWND hwnd, int cmd)
{
    (void)hwnd; (void)cmd;
    return FALSE;
}

HWND GetConsoleWindow(void) { return NULL; }

DWORD GetConsoleProcessList(LPDWORD list, DWORD count)
{
    (void)list; (void)count;
    return 0;
}

static FILETIME ft_from_100ns(ULONGLONG t)
{
    FILETIME ft = { (DWORD)t, (DWORD)(t >> 32) };
    return ft;
}

/* creation time from /proc/self/stat (clock ticks since boot) */
BOOL GetProcessTimes(HANDLE process, LPFILETIME creation, LPFILETIME exit_time,
                     LPFILETIME kernel, LPFILETIME user)
{
    struct rusage ru;
    (void)process;
    if (getrusage(RUSAGE_SELF, &ru) != 0)
        return FALSE;
    if (kernel)
        *kernel = ft_from_100ns((ULONGLONG)ru.ru_stime.tv_sec * 10000000ULL +
                                (ULONGLONG)ru.ru_stime.tv_usec * 10ULL);
    if (user)
        *user = ft_from_100ns((ULONGLONG)ru.ru_utime.tv_sec * 10000000ULL +
                              (ULONGLONG)ru.ru_utime.tv_usec * 10ULL);
    if (exit_time)
        *exit_time = ft_from_100ns(0);
    if (creation) {
        FILETIME now;
        unsigned long long start = 0;
        double uptime = 0;
        char buf[1024];
        FILE *f = fopen("/proc/self/stat", "r");
        if (f) {
            if (fgets(buf, sizeof(buf), f)) {
                char *p = strrchr(buf, ')');
                /* field 22 (starttime); p points at field 2's end */
                for (int field = 2; p && field < 22; field++)
                    p = strchr(p + 1, ' ');
                if (p)
                    start = strtoull(p + 1, NULL, 10);
            }
            fclose(f);
        }
        f = fopen("/proc/uptime", "r");
        if (f) {
            if (fscanf(f, "%lf", &uptime) != 1)
                uptime = 0;
            fclose(f);
        }
        GetSystemTimeAsFileTime(&now);
        ULONGLONG n = ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
        double age = uptime - (double)start / (double)sysconf(_SC_CLK_TCK);
        if (age < 0) age = 0;
        *creation = ft_from_100ns(n - (ULONGLONG)(age * 1e7));
    }
    return TRUE;
}

BOOL IsWindowVisible(HWND hwnd)
{
    (void)hwnd;
    return FALSE;   /* no Win32 windows; the window handover stays off */
}

BOOL AllowSetForegroundWindow(DWORD pid)
{
    (void)pid;
    return TRUE;
}

errno_t _strlwr_s(char *s, size_t size)
{
    if (!s) return EINVAL;
    for (size_t i = 0; i < size && s[i]; i++)
        if (s[i] >= 'A' && s[i] <= 'Z') s[i] = (char)(s[i] + 32);
    return 0;
}

/* ===================================================================== */
/* 16-bit wide strings and MSVC printf semantics                          */
/* ===================================================================== */

/* The runtime is written against MSVC: WCHAR and L"..." are 16 bit, "%S" in
 * a narrow format is a wide string and "%s" in a wide format is a wide string.
 * POSIX builds use -fshort-wchar, so L"..." matches WCHAR; win32_compat.h maps
 * the wcs* names the runtime calls onto these (glibc's assume 4 bytes). */

int xbox_wcscmp(const WCHAR *a, const WCHAR *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)*a - (int)*b;
}

static WCHAR w16_lower(WCHAR c) { return (c >= 'A' && c <= 'Z') ? (WCHAR)(c + 32) : c; }

int xbox_wcsnicmp(const WCHAR *a, const WCHAR *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        WCHAR x = w16_lower(a[i]), y = w16_lower(b[i]);
        if (x != y) return (int)x - (int)y;
        if (!x) return 0;
    }
    return 0;
}

int xbox_wcsicmp(const WCHAR *a, const WCHAR *b) { return xbox_wcsnicmp(a, b, (size_t)-1); }

WCHAR *xbox_wcschr(const WCHAR *s, WCHAR c)
{
    for (;; s++) {
        if (*s == c) return (WCHAR *)s;
        if (!*s) return NULL;
    }
}

WCHAR *xbox_wcsrchr(const WCHAR *s, WCHAR c)
{
    const WCHAR *hit = NULL;
    for (;; s++) {
        if (*s == c) hit = s;
        if (!*s) return (WCHAR *)hit;
    }
}

errno_t xbox_wcsncpy_s(WCHAR *dst, size_t size, const WCHAR *src, size_t count)
{
    if (!dst || !size) return EINVAL;
    size_t n = 0;
    while (n < count && src[n]) n++;
    if (n >= size) {
        if (count != _TRUNCATE) { dst[0] = 0; return ERANGE; }
        n = size - 1;
    }
    memcpy(dst, src, n * sizeof(WCHAR));
    dst[n] = 0;
    return 0;
}

errno_t xbox_wcscpy_s(WCHAR *dst, size_t size, const WCHAR *src)
{
    if (!dst || !size) return EINVAL;
    size_t n = xbox_wcslen(src);
    if (n >= size) { dst[0] = 0; return ERANGE; }
    memcpy(dst, src, (n + 1) * sizeof(WCHAR));
    return 0;
}

errno_t xbox_wcscat_s(WCHAR *dst, size_t size, const WCHAR *src)
{
    if (!dst || !size) return EINVAL;
    size_t d = xbox_wcslen(dst);
    if (d >= size) return EINVAL;
    return xbox_wcscpy_s(dst + d, size - d, src);
}

/* UTF-8 copy of a 16-bit string in a fresh malloc'd buffer */
static char *w16_to_utf8(const WCHAR *w)
{
    if (!w) return strdup("(null)");
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = (char *)malloc(n > 0 ? (size_t)n : 1);
    if (n <= 0 || !WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL))
        s[0] = 0;
    return s;
}

typedef struct { char *buf; size_t size, len; } w32_out;

static void out_put(w32_out *o, const char *s, size_t n)
{
    if (o->size && o->len + 1 < o->size) {
        size_t room = o->size - o->len - 1;
        memcpy(o->buf + o->len, s, n < room ? n : room);
    }
    o->len += n;
}

/* format one argument with glibc into o (spec is a complete narrow spec);
 * arg is evaluated once -- it is often a va_arg */
#define OUT_FMT(o, spec, arg) do {                              \
        __typeof__(arg) v_ = (arg);                             \
        int n_ = snprintf(NULL, 0, spec, v_);                   \
        if (n_ > 0) {                                           \
            char *t_ = (char *)malloc((size_t)n_ + 1);          \
            snprintf(t_, (size_t)n_ + 1, spec, v_);             \
            out_put(o, t_, (size_t)n_);                         \
            free(t_);                                           \
        }                                                       \
    } while (0)

/* vsnprintf with the MSVC conversions on top of glibc's: wide_s says whether
 * a bare %s / %c is wide (wide-format functions) or narrow. Also accepts the
 * MSVC I64/I32/I length prefixes. Returns the full length, like vsnprintf. */
static int w32_vformat(char *buf, size_t size, const char *fmt, int wide_s, va_list ap)
{
    w32_out out = { buf, size, 0 }, *o = &out;
    while (*fmt) {
        if (*fmt != '%') {
            const char *e = strchr(fmt, '%');
            size_t n = e ? (size_t)(e - fmt) : strlen(fmt);
            out_put(o, fmt, n);
            fmt += n;
            continue;
        }
        if (fmt[1] == '%') { out_put(o, "%", 1); fmt += 2; continue; }
        /* %[flags][width][.prec][len]conv, rebuilt for glibc */
        const char *start = fmt, *p = fmt + 1;
        char spec[64];
        size_t k = 0;
        spec[k++] = '%';
        while (*p && strchr("-+ #0", *p) && k < 16) spec[k++] = *p++;
        if (*p == '*') {
            k += (size_t)snprintf(spec + k, sizeof(spec) - k, "%d", va_arg(ap, int));
            p++;
        } else {
            while (*p >= '0' && *p <= '9' && k < 32) spec[k++] = *p++;
        }
        if (*p == '.') {
            spec[k++] = *p++;
            if (*p == '*') {
                k += (size_t)snprintf(spec + k, sizeof(spec) - k, "%d", va_arg(ap, int));
                p++;
            } else {
                while (*p >= '0' && *p <= '9' && k < 48) spec[k++] = *p++;
            }
        }
        /* length: 0 none, 1 h, 2 hh, 3 l, 4 ll, 5 z, 6 j, 7 t, 8 L */
        int len = 0;
        if (p[0] == 'I' && p[1] == '6' && p[2] == '4') { len = 4; p += 3; }
        else if (p[0] == 'I' && p[1] == '3' && p[2] == '2') { p += 3; }
        else if (p[0] == 'I') { len = 5; p++; }
        else if (p[0] == 'h' && p[1] == 'h') { len = 2; p += 2; }
        else if (p[0] == 'h') { len = 1; p++; }
        else if (p[0] == 'l' && p[1] == 'l') { len = 4; p += 2; }
        else if (p[0] == 'l') { len = 3; p++; }
        else if (*p == 'z') { len = 5; p++; }
        else if (*p == 'j') { len = 6; p++; }
        else if (*p == 't') { len = 7; p++; }
        else if (*p == 'L') { len = 8; p++; }
        const char conv = *p;
        if (!conv) { out_put(o, start, strlen(start)); break; }
        fmt = p + 1;
        switch (conv) {
        case 's': case 'S': {
            int wide = (conv == 'S') ? !wide_s : wide_s;
            if (len == 1) wide = 0;
            if (len == 3) wide = 1;
            spec[k++] = 's'; spec[k] = 0;
            if (wide) {
                char *u = w16_to_utf8(va_arg(ap, const WCHAR *));
                OUT_FMT(o, spec, (const char *)u);
                free(u);
            } else {
                const char *a = va_arg(ap, const char *);
                OUT_FMT(o, spec, a ? a : "(null)");
            }
            break;
        }
        case 'c': case 'C': {
            int wide = (conv == 'C') ? !wide_s : wide_s;
            if (len == 1) wide = 0;
            if (len == 3) wide = 1;
            WCHAR w[2] = { (WCHAR)va_arg(ap, int), 0 };
            char u[8] = { (char)w[0], 0 };
            if (wide && !WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof(u), NULL, NULL))
                u[0] = '?', u[1] = 0;
            spec[k++] = 's'; spec[k] = 0;
            OUT_FMT(o, spec, (const char *)u);
            break;
        }
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
            static const char *const lm[] = { "", "h", "hh", "l", "ll", "z", "j", "t", "" };
            snprintf(spec + k, sizeof(spec) - k, "%s%c", lm[len], conv);
            if (len == 4)      OUT_FMT(o, spec, va_arg(ap, long long));
            else if (len == 3) OUT_FMT(o, spec, va_arg(ap, long));
            else if (len == 5) OUT_FMT(o, spec, va_arg(ap, size_t));
            else if (len == 6) OUT_FMT(o, spec, va_arg(ap, intmax_t));
            else if (len == 7) OUT_FMT(o, spec, va_arg(ap, ptrdiff_t));
            else               OUT_FMT(o, spec, va_arg(ap, int));
            break;
        }
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A':
            if (len == 8) {
                spec[k++] = 'L'; spec[k++] = conv; spec[k] = 0;
                OUT_FMT(o, spec, va_arg(ap, long double));
            } else {
                spec[k++] = conv; spec[k] = 0;
                OUT_FMT(o, spec, va_arg(ap, double));
            }
            break;
        case 'p':
            spec[k++] = 'p'; spec[k] = 0;
            OUT_FMT(o, spec, va_arg(ap, void *));
            break;
        case 'n':
            (void)va_arg(ap, int *);
            break;
        default:
            out_put(o, start, (size_t)(fmt - start));
            break;
        }
    }
    if (out.size) out.buf[out.len < out.size ? out.len : out.size - 1] = 0;
    return (int)out.len;
}
#undef OUT_FMT

int w32_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    return w32_vformat(buf, size, fmt, 0, ap);
}

int w32_vfprintf(FILE *f, const char *fmt, va_list ap)
{
    char small[1024];
    va_list ap2;
    va_copy(ap2, ap);
    int n = w32_vformat(small, sizeof(small), fmt, 0, ap);
    if (n >= (int)sizeof(small)) {
        char *big = (char *)malloc((size_t)n + 1);
        w32_vformat(big, (size_t)n + 1, fmt, 0, ap2);
        fputs(big, f);
        free(big);
    } else {
        fputs(small, f);
    }
    va_end(ap2);
    return n;
}

int w32_fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = w32_vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int xbox_vswprintf_s(WCHAR *buf, size_t size, const WCHAR *fmt, va_list ap)
{
    if (!buf || !size || !fmt) return -1;
    char *f8 = w16_to_utf8(fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = w32_vformat(NULL, 0, f8, 1, ap);
    char *u = (char *)malloc((size_t)n + 1);
    w32_vformat(u, (size_t)n + 1, f8, 1, ap2);
    va_end(ap2);
    int w = MultiByteToWideChar(CP_UTF8, 0, u, -1, NULL, 0);
    int ret = -1;
    if (w > 0 && (size_t)w <= size) {
        MultiByteToWideChar(CP_UTF8, 0, u, -1, buf, (int)size);
        ret = w - 1;
    } else {
        buf[0] = 0;
    }
    free(u);
    free(f8);
    return ret;
}

int xbox_swprintf_s(WCHAR *buf, size_t size, const WCHAR *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = xbox_vswprintf_s(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

#endif /* !_WIN32 */
