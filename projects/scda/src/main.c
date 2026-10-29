#include <stdio.h>
#include "scda_options.h"
#include "scda_layout.h"
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <io.h>
#include <fcntl.h>
#include <share.h>
#include <tlhelp32.h>
#endif

#include <xbox/xboxrecomp.h>
#include "recomp_funcs.h"

#define SCDA_XBE_PATH "game_files\\scda\\default.xbe"
#define SCDA_GAME_DIR "game_files\\scda"
#define SCDA_ENTRY_POINT 0x00054775u

/* g_esp: thread-local, declared by recomp_types.h (via recomp_funcs.h) */
extern void scda_dump_actorlist_ring(void);
extern void scda_dump_icall_ring(void);
extern void scda_dump_fade_ring(void);
extern void xbox_PinGuestThread(void);
extern int xbox_GuestCpu(void);

#ifdef _WIN32
static char g_scda_exe_dir[MAX_PATH];
/* Where logs, launch_data.bin and crash dumps go: the EXE's folder, or the
 * release layout's "user" folder (scda_layout.c). */
static char g_scda_data_dir[MAX_PATH];

/* xbox_path_init takes UTF-8; GetModuleFileNameA paths are in the ANSI code
 * page (differs for folder names like "Jörg"). */
static void scda_acp_to_utf8(const char *in, char *out, size_t out_size)
{
    WCHAR wide[MAX_PATH];
    if (!MultiByteToWideChar(CP_ACP, 0, in, -1, wide, MAX_PATH) ||
        !WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)out_size, NULL,
                             NULL))
        snprintf(out, out_size, "%s", in);
}

static void scda_path_in_exe_dir(char *out, size_t out_size, const char *name)
{
    snprintf(out, out_size, "%s\\%s",
             g_scda_data_dir[0] ? g_scda_data_dir : g_scda_exe_dir, name);
}

static void scda_init_exe_dir(void)
{
    DWORD length = GetModuleFileNameA(NULL, g_scda_exe_dir, MAX_PATH);
    char *separator;

    if (!length || length >= MAX_PATH) {
        strcpy_s(g_scda_exe_dir, sizeof(g_scda_exe_dir), ".");
        return;
    }
    separator = strrchr(g_scda_exe_dir, '\\');
    if (separator)
        *separator = '\0';
    else
        strcpy_s(g_scda_exe_dir, sizeof(g_scda_exe_dir), ".");
}

static LONG WINAPI scda_write_crash_dump(EXCEPTION_POINTERS *exception)
{
    static volatile LONG dumping;
    SYSTEMTIME now;
    char dump_name[96];
    char dump_path[MAX_PATH];
    HANDLE dump_file;
    MINIDUMP_EXCEPTION_INFORMATION dump_info;
    BOOL written;

    if (InterlockedCompareExchange(&dumping, 1, 0) != 0)
        return EXCEPTION_EXECUTE_HANDLER;

    fprintf(stderr,
            "[SCDA-CRASH] exception=0x%08lX address=%p tid=%lu host_rip=%p "
            "guest_esp=0x%08X",
            exception->ExceptionRecord->ExceptionCode,
            exception->ExceptionRecord->ExceptionAddress,
            GetCurrentThreadId(),
            (void *)(uintptr_t)exception->ContextRecord->Rip,
            (unsigned)g_esp);
    if (exception->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
            exception->ExceptionRecord->NumberParameters >= 2) {
        fprintf(stderr, " access=%llu fault=%p",
                (unsigned long long)exception->ExceptionRecord->ExceptionInformation[0],
                (void *)(uintptr_t)exception->ExceptionRecord->ExceptionInformation[1]);
    }
    fputc('\n', stderr);
    fflush(stderr);

    /* Host stack overflow: the call chain *is* the finding (generated code
     * calls guest functions as C calls, so a recursion cycle shows up as a
     * repeating group of sub_XXXXXXXX frames). Walk the faulting context
     * with the unwind data, print exe offsets (resolve: cdb -z exe,
     * `ln scda_recomp+<off>`), and skip dbgcore, which needs far more stack
     * than the guarantee set in xbox_PinGuestThread. docs Update 353. */
    if (exception->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
        CONTEXT w = *exception->ContextRecord;
        uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
        int depth;
        fprintf(stderr, "[SCDA-STACKOVF] exe base=%p, innermost frames:\n", (void *)base);
        for (depth = 0; depth < 96 && w.Rip; depth++) {
            DWORD64 image = 0, frame = 0;
            PVOID hd = NULL;
            PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(w.Rip, &image, NULL);
            if (w.Rip >= base && w.Rip < base + 0x10000000)
                fprintf(stderr, "[SCDA-STACKOVF]   #%02d exe+0x%llX\n", depth,
                        (unsigned long long)(w.Rip - base));
            else
                fprintf(stderr, "[SCDA-STACKOVF]   #%02d %p\n", depth, (void *)w.Rip);
            if (!rf) {
                w.Rip = *(DWORD64 *)w.Rsp;
                w.Rsp += 8;
                continue;
            }
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, w.Rip, rf, &w, &hd, &frame, NULL);
        }
        fflush(stderr);
        return EXCEPTION_EXECUTE_HANDLER;
    }

    /* Preserve the inexpensive host-side diagnostic rings before dbgcore is
     * entered.  MiniDumpWriteDump itself faulted in the 2026-09-27 14:56 run,
     * leaving a zero-byte dump and previously losing all of this evidence. */
    scda_dump_fade_ring();
    scda_dump_actorlist_ring();
    scda_dump_icall_ring();

    GetLocalTime(&now);
    snprintf(dump_name, sizeof(dump_name),
             "SCDA_crash_%04u-%02u-%02u_%02u-%02u-%02u.dmp",
             now.wYear, now.wMonth, now.wDay,
             now.wHour, now.wMinute, now.wSecond);
    scda_path_in_exe_dir(dump_path, sizeof(dump_path), dump_name);
    dump_file = CreateFileA(dump_path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (dump_file == INVALID_HANDLE_VALUE)
        return EXCEPTION_EXECUTE_HANDLER;

    dump_info.ThreadId = GetCurrentThreadId();
    dump_info.ExceptionPointers = exception;
    dump_info.ClientPointers = FALSE;
    written = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                                dump_file,
                                (MINIDUMP_TYPE)(MiniDumpNormal |
                                                MiniDumpWithThreadInfo),
                                &dump_info, NULL, NULL);
    CloseHandle(dump_file);
    if (written)
        fprintf(stderr, "[SCDA-CRASH] Minidump written: %s\n", dump_path);
    else
        fprintf(stderr, "[SCDA-CRASH] Minidump failed (Win32 error %lu)\n",
                GetLastError());
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

/* Fail-fast exits (0xC0000409: CRT invalid parameter, abort()) bypass the
 * unhandled-exception filter above, so the process vanished without a dump
 * or a log line (docs Update 344). Log the host call stack as exe offsets
 * (resolve offline: `ln scda_recomp+<off>` on the exe) before the CRT
 * terminates. */
static void scda_log_host_backtrace(const char *why)
{
    void *frames[48];
    USHORT n = RtlCaptureStackBackTrace(1, 48, frames, NULL);
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    USHORT i;

    fprintf(stderr, "[SCDA-FAILFAST] %s tid=%lu guest_esp=0x%08X frames=%u\n",
            why, GetCurrentThreadId(), (unsigned)g_esp, (unsigned)n);
    for (i = 0; i < n; i++) {
        uintptr_t a = (uintptr_t)frames[i];
        HMODULE mod = NULL;
        char name[MAX_PATH] = "?";
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)a, &mod);
        if (mod)
            GetModuleFileNameA(mod, name, sizeof(name));
        fprintf(stderr, "[SCDA-FAILFAST]   #%02u %p %s+0x%llX\n", i, (void *)a,
                strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name,
                (unsigned long long)(a - (mod ? (uintptr_t)mod : base)));
    }
    fflush(stderr);
}

static void scda_invalid_parameter(const wchar_t *expr, const wchar_t *func,
                                   const wchar_t *file, unsigned int line,
                                   uintptr_t reserved)
{
    (void)expr; (void)func; (void)file; (void)line; (void)reserved;
    scda_log_host_backtrace("CRT invalid parameter");
    abort();
}

static void scda_on_sigabrt(int sig)
{
    (void)sig;
    scda_log_host_backtrace("abort()");
}

/* Sampling profiler for the guest main thread (docs Update 345).
 * RECOMP_SAMPLE_MAIN="start_s,duration_s": after start_s seconds, sample the
 * main thread's RIP every ~2 ms for duration_s seconds, then print the
 * hottest exe offsets (resolve offline with `ln scda_recomp+<off>`). */
static HANDLE g_scda_main_thread;
#define SCDA_SAMPLES 16384

static int scda_cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static DWORD WINAPI scda_sampler_thread(LPVOID param)
{
    static uint64_t rips[SCDA_SAMPLES];
    const char *spec = (const char *)param;
    double start_s = atof(spec), dur_s = 10.0;
    const char *comma = strchr(spec, ',');
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    unsigned n = 0, i, other = 0;
    DWORD t_end;

    if (comma)
        dur_s = atof(comma + 1);
    Sleep((DWORD)(start_s * 1000.0));
    t_end = GetTickCount() + (DWORD)(dur_s * 1000.0);
    while (GetTickCount() < t_end && n < SCDA_SAMPLES) {
        CONTEXT ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_FULL;
        if (SuspendThread(g_scda_main_thread) != (DWORD)-1) {
            BOOL ok = GetThreadContext(g_scda_main_thread, &ctx);
            DWORD64 chain[48];
            int depth = 0, d;
            /* Every 150th sample: the whole call chain, walked while the
             * thread is still suspended. Generated code calls guest
             * functions as plain C calls, so these frames are the guest
             * call chain (resolve the offsets with cdb `ln`). Nothing that
             * takes a CRT lock may run until ResumeThread: the main thread
             * can be suspended inside fprintf holding the stderr lock, and
             * printing here then deadlocked the game (docs Update 474). */
            if (ok && n % 150 == 0) {
                CONTEXT w = ctx;
                for (depth = 0; depth < 48 && w.Rip; depth++) {
                    DWORD64 image = 0, frame = 0;
                    PVOID hd = NULL;
                    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(w.Rip, &image, NULL);
                    chain[depth] = w.Rip;
                    if (!rf) {
                        w.Rip = *(DWORD64 *)w.Rsp;
                        w.Rsp += 8;
                        continue;
                    }
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, w.Rip, rf, &w, &hd, &frame, NULL);
                }
            }
            ResumeThread(g_scda_main_thread);
            if (depth) {
                fprintf(stderr, "[SAMPLE-STACK] sample %u:", n);
                for (d = 0; d < depth; d++) {
                    if (chain[d] >= base && chain[d] < base + 0x10000000)
                        fprintf(stderr, " exe+0x%llX", (unsigned long long)(chain[d] - base));
                    else
                        fprintf(stderr, " %p", (void *)chain[d]);
                }
                fprintf(stderr, "\n");
            }
            if (ok) {
                /* Bucket by function start (x64 unwind data); leaf
                 * functions without an entry keep their raw RIP. */
                DWORD64 image = 0;
                PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(ctx.Rip, &image, NULL);
                rips[n++] = rf ? image + rf->BeginAddress : ctx.Rip;
            }
        }
        Sleep(2);
    }
    qsort(rips, n, sizeof(rips[0]), scda_cmp_u64);
    fprintf(stderr, "[SAMPLE] %u main-thread samples over %.1f s (from %.1f s); "
            "RIPs with >= 0.5%%:\n", n, dur_s, start_s);
    for (i = 0; i < n;) {
        unsigned j = i;
        while (j < n && rips[j] == rips[i])
            j++;
        if ((j - i) * 200 >= n) {
            if (rips[i] >= base && rips[i] < base + 0x10000000)
                fprintf(stderr, "[SAMPLE] %5.1f%% exe+0x%llX\n",
                        100.0 * (j - i) / n,
                        (unsigned long long)(rips[i] - base));
            else
                fprintf(stderr, "[SAMPLE] %5.1f%% %p (outside exe)\n",
                        100.0 * (j - i) / n, (void *)rips[i]);
        } else {
            other += j - i;
        }
        i = j;
    }
    fprintf(stderr, "[SAMPLE] %5.1f%% in RIPs below 0.5%% each\n",
            n ? 100.0 * other / n : 0.0);
    fflush(stderr);
    return 0;
}

/* RECOMP_SAMPLE_ALL="start_s,duration_s" (docs Update 474): the same RIP
 * sampling for EVERY thread of the process (guest threads, PFIFO/GPU,
 * audio, window), to see which one limits the frame rate. Per thread the
 * hottest functions (>= 2 %) as exe offsets or module+offset. Between
 * SuspendThread and ResumeThread only GetThreadContext runs -- anything
 * taking a lock there can deadlock against the suspended thread. */
#define SCDA_ALL_THREADS 64
#define SCDA_ALL_SAMPLES 4096
#define SCDA_INCL_SLOTS 2048

static void scda_print_rip(const char *prefix, double pct, uint64_t rip, uintptr_t base)
{
    HMODULE mod = NULL;
    char name[MAX_PATH] = "?", *slash;
    if (rip >= base && rip < base + 0x10000000) {
        fprintf(stderr, "%s %5.1f%% exe+0x%llX\n", prefix, pct,
                (unsigned long long)(rip - base));
        return;
    }
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)rip, &mod) && mod)
        GetModuleFileNameA(mod, name, sizeof name);
    slash = strrchr(name, '\\');
    fprintf(stderr, "%s %5.1f%% %s+0x%llX\n", prefix, pct, slash ? slash + 1 : name,
            (unsigned long long)(rip - (uintptr_t)mod));
}

/* Other processes' CPU time over the sampling window (docs Update 646): the
 * player's slow spot ran with ~7 more busy cores than the automated runs. */
#define SCDA_PROC_MAX 1024
typedef struct { DWORD pid; ULONGLONG t; char name[40]; } ScdaProcTime;

static unsigned scda_proc_snapshot(ScdaProcTime *out)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe;
    unsigned n = 0;
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    pe.dwSize = sizeof pe;
    for (BOOL ok = Process32First(snap, &pe); ok && n < SCDA_PROC_MAX;
         ok = Process32Next(snap, &pe)) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                               pe.th32ProcessID);
        FILETIME c, e, k, u;
        if (!h)
            continue;
        if (GetProcessTimes(h, &c, &e, &k, &u)) {
            out[n].pid = pe.th32ProcessID;
            out[n].t = (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) +
                       (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
            snprintf(out[n].name, sizeof out[n].name, "%s", pe.szExeFile);
            n++;
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    return n;
}

static void scda_proc_report(const ScdaProcTime *a, unsigned na,
                             const ScdaProcTime *b, unsigned nb, double wall_100ns)
{
    for (int round = 0; round < 12; round++) {
        double best = 0;
        int bi = -1;
        for (unsigned i = 0; i < nb; i++) {
            double d = 0;
            if (!b[i].pid || !b[i].name[0])
                continue;
            for (unsigned j = 0; j < na; j++)
                if (a[j].pid == b[i].pid) {
                    d = (double)(b[i].t - a[j].t);
                    break;
                }
            if (d > best) {
                best = d;
                bi = (int)i;
            }
        }
        if (bi < 0 || best * 100.0 / wall_100ns < 2.0)
            break;
        fprintf(stderr, "[SAMPLE-PROC] %s (pid %lu): %.1f%% of one core\n",
                b[bi].name, (unsigned long)b[bi].pid, best * 100.0 / wall_100ns);
        ((ScdaProcTime *)b)[bi].name[0] = 0;   /* reported */
    }
}

static DWORD WINAPI scda_sampler_all_thread(LPVOID param)
{
    const char *spec = (const char *)param;
    double start_s = atof(spec), dur_s = 10.0;
    const char *comma = strchr(spec, ',');
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    HANDLE th[SCDA_ALL_THREADS];
    DWORD tid[SCDA_ALL_THREADS], self = GetCurrentThreadId(), t_end;
    unsigned cnt[SCDA_ALL_THREADS] = {0}, walks[SCDA_ALL_THREADS] = {0};
    unsigned nth = 0, k, rounds = 0;
    uint64_t *rips, *incl;
    uintptr_t ntdll_lo = 0, ntdll_hi = 0;
    uintptr_t ucrt_lo = 0, ucrt_hi = 0, exe_size = 0;
    static uint64_t crt_caller[64][2];
    /* Thread parked in ntdll (a lock/condition wait, docs Update 643): the
     * first two exe frames above the wait, per thread. */
    static uint64_t wait_site[256][8];   /* k, f0, f1, count, f2..f5 */

    if (comma)
        dur_s = atof(comma + 1);
    Sleep((DWORD)(start_s * 1000.0));
    rips = (uint64_t *)malloc(sizeof(uint64_t) * SCDA_ALL_THREADS * SCDA_ALL_SAMPLES);
    if (!rips)
        return 0;
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te;
        te.dwSize = sizeof te;
        for (BOOL ok = Thread32First(snap, &te); ok && nth < SCDA_ALL_THREADS;
             ok = Thread32Next(snap, &te)) {
            HANDLE h;
            if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self)
                continue;
            h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                           THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
            if (h) {
                tid[nth] = te.th32ThreadID;
                th[nth++] = h;
            }
        }
        CloseHandle(snap);
    }
    {
        /* ntdll's extent from its PE header, to skip threads in a wait. */
        const uint8_t *nt = (const uint8_t *)GetModuleHandleA("ntdll.dll");
        if (nt) {
            const IMAGE_NT_HEADERS *nh =
                (const IMAGE_NT_HEADERS *)(nt + ((const IMAGE_DOS_HEADER *)nt)->e_lfanew);
            ntdll_lo = (uintptr_t)nt;
            ntdll_hi = ntdll_lo + nh->OptionalHeader.SizeOfImage;
        }
    }
    incl = (uint64_t *)calloc((size_t)SCDA_ALL_THREADS * SCDA_INCL_SLOTS * 2, sizeof(uint64_t));
    {
        HMODULE uc = GetModuleHandleA("ucrtbase.dll");
        if (uc) {
            IMAGE_NT_HEADERS *uh = (IMAGE_NT_HEADERS *)((uint8_t *)uc +
                ((IMAGE_DOS_HEADER *)uc)->e_lfanew);
            ucrt_lo = (uintptr_t)uc;
            ucrt_hi = ucrt_lo + uh->OptionalHeader.SizeOfImage;
        }
        IMAGE_NT_HEADERS *eh = (IMAGE_NT_HEADERS *)((uint8_t *)base +
            ((IMAGE_DOS_HEADER *)base)->e_lfanew);
        exe_size = eh->OptionalHeader.SizeOfImage;
    }
    /* Per-thread CPU time over the window (docs Update 646): which thread
     * is busy and which only waits, for F9 at the player's slow spot. */
    static ULONGLONG cpu0[SCDA_ALL_THREADS], cpu_user0[SCDA_ALL_THREADS];
    for (k = 0; k < nth; k++) {
        FILETIME c, e, kt, ut;
        if (GetThreadTimes(th[k], &c, &e, &kt, &ut)) {
            cpu_user0[k] = ((ULONGLONG)ut.dwHighDateTime << 32) | ut.dwLowDateTime;
            cpu0[k] = cpu_user0[k] + (((ULONGLONG)kt.dwHighDateTime << 32) | kt.dwLowDateTime);
        }
    }
    /* Per logical processor busy time over the window (docs Update 646):
     * shows whether other work shares the guest core's SMT partner. */
    typedef struct { LARGE_INTEGER idle, kernel, user, dpc, irq; ULONG irqs; } ScdaCpuPerf;
    typedef LONG (NTAPI *ScdaNtQsi)(ULONG, PVOID, ULONG, PULONG);
    static ScdaCpuPerf perf0[64], perf1[64];
    ScdaNtQsi qsi = (ScdaNtQsi)GetProcAddress(GetModuleHandleA("ntdll.dll"),
                                              "NtQuerySystemInformation");
    ULONG perf_len = 0;
    if (qsi && qsi(8, perf0, sizeof perf0, &perf_len) != 0)
        perf_len = 0;
    static ScdaProcTime procs0[SCDA_PROC_MAX], procs1[SCDA_PROC_MAX];
    unsigned nprocs0 = scda_proc_snapshot(procs0);
    DWORD tick0 = GetTickCount();
    t_end = GetTickCount() + (DWORD)(dur_s * 1000.0);
    while (GetTickCount() < t_end) {
        for (k = 0; k < nth; k++) {
            CONTEXT ctx;
            BOOL ok;
            DWORD64 chain[32];
            int depth = 0, d;
            if (cnt[k] >= SCDA_ALL_SAMPLES)
                continue;
            memset(&ctx, 0, sizeof(ctx));
            ctx.ContextFlags = CONTEXT_FULL;
            if (SuspendThread(th[k]) == (DWORD)-1)
                continue;
            ok = GetThreadContext(th[k], &ctx);
            /* Every 4th round, for a thread not parked in ntdll (a wait),
             * the call chain -- walked while suspended, printed nowhere
             * until all threads run again. Driver leaves (nvoglv64, win32u)
             * are included: those are the GPU sync points. */
            if (ok && incl && (rounds & 3) == 0 &&
                    !(ctx.Rip >= ntdll_lo && ctx.Rip < ntdll_hi)) {
                CONTEXT w = ctx;
                for (depth = 0; depth < 32 && w.Rip; depth++) {
                    DWORD64 image = 0, frame = 0;
                    PVOID hd = NULL;
                    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(w.Rip, &image, NULL);
                    chain[depth] = rf ? image + rf->BeginAddress : w.Rip;
                    if (!rf)
                        break;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, w.Rip, rf, &w, &hd,
                                     &frame, NULL);
                }
            }
            if (ok && (rounds & 3) == 1 &&
                    ctx.Rip >= ntdll_lo && ctx.Rip < ntdll_hi) {
                CONTEXT w = ctx;
                uint64_t f[6] = {0, 0, 0, 0, 0, 0};
                int nf = 0, dw;
                for (dw = 0; dw < 64 && w.Rip && nf < 6; dw++) {
                    DWORD64 image = 0, frame = 0;
                    PVOID hd = NULL;
                    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(w.Rip, &image, NULL);
                    if (!rf)
                        break;
                    if (w.Rip >= base && w.Rip < base + exe_size)
                        f[nf++] = image + rf->BeginAddress;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, w.Rip, rf, &w, &hd,
                                     &frame, NULL);
                }
                if (nf) {
                    unsigned c;
                    for (c = 0; c < 256; c++) {
                        if (wait_site[c][3] == 0 || (wait_site[c][0] == k &&
                                wait_site[c][1] == f[0] && wait_site[c][2] == f[1])) {
                            wait_site[c][0] = k;
                            wait_site[c][1] = f[0];
                            wait_site[c][2] = f[1];
                            wait_site[c][3]++;
                            for (int q = 2; q < 6; q++)
                                wait_site[c][q + 2] = f[q];
                            break;
                        }
                    }
                }
            }
            ResumeThread(th[k]);
            if (ok)
                rips[k * SCDA_ALL_SAMPLES + cnt[k]++] = ctx.Rip;
            /* Leaf in the C runtime (getenv & co., docs Update 637): which
             * exe function called it -- the first exe frame of the walk. */
            if (depth > 1 && ucrt_lo && chain[0] >= ucrt_lo && chain[0] < ucrt_hi) {
                for (d = 1; d < depth; d++) {
                    if (chain[d] >= base && chain[d] < base + exe_size) {
                        unsigned c;
                        for (c = 0; c < 64; c++) {
                            if (crt_caller[c][0] == chain[d] || crt_caller[c][0] == 0) {
                                crt_caller[c][0] = chain[d];
                                crt_caller[c][1]++;
                                break;
                            }
                        }
                        break;
                    }
                }
            }
            if (depth) {
                uint64_t *tab = incl + (size_t)k * SCDA_INCL_SLOTS * 2;
                walks[k]++;
                for (d = 0; d < depth; d++) {
                    uint64_t f = chain[d];
                    unsigned h = (unsigned)((f >> 4) * 2654435761u) % SCDA_INCL_SLOTS, p;
                    int seen = 0, e;
                    for (e = 0; e < d; e++)
                        if (chain[e] == f)
                            seen = 1;        /* recursion: count once per walk */
                    if (seen)
                        continue;
                    for (p = 0; p < SCDA_INCL_SLOTS; p++) {
                        unsigned s = (h + p) % SCDA_INCL_SLOTS;
                        if (tab[2 * s] == f || tab[2 * s] == 0) {
                            tab[2 * s] = f;
                            tab[2 * s + 1]++;
                            break;
                        }
                    }
                }
            }
        }
        rounds++;
        Sleep(2);
    }
    fprintf(stderr, "[SAMPLE-ALL] %u threads, %u rounds over %.1f s (from %.1f s)\n",
            nth, rounds, dur_s, start_s);
    {
        unsigned nprocs1 = scda_proc_snapshot(procs1);
        scda_proc_report(procs0, nprocs0, procs1, nprocs1,
                         (double)(GetTickCount() - tick0) * 10000.0);
    }
    if (perf_len && qsi(8, perf1, sizeof perf1, &perf_len) == 0) {
        unsigned ncpu = perf_len / sizeof(ScdaCpuPerf), c;
        double rest = 0;
        unsigned guest = (unsigned)xbox_GuestCpu(), partner = guest ^ 1u;
        for (c = 0; c < ncpu; c++) {
            double total = (double)(perf1[c].kernel.QuadPart - perf0[c].kernel.QuadPart +
                                    perf1[c].user.QuadPart - perf0[c].user.QuadPart);
            double idle = (double)(perf1[c].idle.QuadPart - perf0[c].idle.QuadPart);
            double busy = total > 0 ? 100.0 * (1.0 - idle / total) : 0;
            if (c == guest || c == partner)
                fprintf(stderr, "[SAMPLE-LP] logical processor %u busy %.1f%%%s\n", c, busy,
                        c == guest ? " (guest core)" : " (its SMT partner)");
            else
                rest += busy;
        }
        if (ncpu > 2)
            fprintf(stderr, "[SAMPLE-LP] other %u logical processors busy %.1f%% on average\n",
                    ncpu - 2, rest / (ncpu - 2));
        /* Interrupt and DPC time belongs to no process (mouse polling,
         * USB, audio, GPU drivers): summed over all processors, in cores. */
        {
            double dpc = 0, irq = 0, irqs = 0;
            double wall = (double)(GetTickCount() - tick0) * 10000.0;
            for (c = 0; c < ncpu; c++) {
                dpc += (double)(perf1[c].dpc.QuadPart - perf0[c].dpc.QuadPart);
                irq += (double)(perf1[c].irq.QuadPart - perf0[c].irq.QuadPart);
                irqs += (double)(perf1[c].irqs - perf0[c].irqs);
            }
            if (wall > 0)
                fprintf(stderr, "[SAMPLE-LP] interrupts %.0f per second, interrupt time "
                        "%.2f cores, DPC time %.2f cores (CPU0: irq %.1f%% dpc %.1f%%)\n",
                        irqs * 10000000.0 / wall, irq / wall, dpc / wall,
                        100.0 * (double)(perf1[0].irq.QuadPart - perf0[0].irq.QuadPart) / wall,
                        100.0 * (double)(perf1[0].dpc.QuadPart - perf0[0].dpc.QuadPart) / wall);
        }
    }
    {
        double wall = (double)(GetTickCount() - tick0) * 10000.0;   /* 100 ns */
        for (k = 0; k < nth; k++) {
            FILETIME c, e, kt, ut;
            if (wall > 0 && GetThreadTimes(th[k], &c, &e, &kt, &ut)) {
                ULONGLONG u = ((ULONGLONG)ut.dwHighDateTime << 32) | ut.dwLowDateTime;
                ULONGLONG t = u + (((ULONGLONG)kt.dwHighDateTime << 32) | kt.dwLowDateTime);
                double pct = 100.0 * (double)(t - cpu0[k]) / wall;
                if (pct >= 2.0)
                    fprintf(stderr, "[SAMPLE-CPU] thread %lu: %.1f%% of one core (user %.1f%%)\n",
                            (unsigned long)tid[k], pct,
                            100.0 * (double)(u - cpu_user0[k]) / wall);
            }
        }
    }
    for (k = 0; k < nth; k++) {
        uint64_t *r = rips + (size_t)k * SCDA_ALL_SAMPLES;
        unsigned n = cnt[k], i;
        PWSTR desc = NULL;
        char dname[64] = "";
        if (!n)
            continue;
        /* Bucket by function start (x64 unwind data); after all threads
         * are running again. */
        for (i = 0; i < n; i++) {
            DWORD64 image = 0;
            PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(r[i], &image, NULL);
            if (rf)
                r[i] = image + rf->BeginAddress;
        }
        qsort(r, n, sizeof(r[0]), scda_cmp_u64);
        if (SUCCEEDED(GetThreadDescription(th[k], &desc)) && desc) {
            WideCharToMultiByte(CP_ACP, 0, desc, -1, dname, sizeof dname, NULL, NULL);
            LocalFree(desc);
        }
        fprintf(stderr, "[SAMPLE-ALL] thread %lu %s: %u samples\n",
                (unsigned long)tid[k], dname, n);
        for (i = 0; i < n;) {
            unsigned j = i;
            while (j < n && r[j] == r[i])
                j++;
            if ((j - i) * 50 >= n)
                scda_print_rip("[SAMPLE-ALL]  ", 100.0 * (j - i) / n, r[i], base);
            i = j;
        }
        /* Inclusive: share of the walked (in-exe) samples each function was
         * on the stack for, >= 5 %; resolve exe+off with cdb `ln`. */
        if (incl && walks[k] >= 20) {
            uint64_t *tab = incl + (size_t)k * SCDA_INCL_SLOTS * 2;
            unsigned s;
            fprintf(stderr, "[SAMPLE-INCL] thread %lu: %u stack walks\n",
                    (unsigned long)tid[k], walks[k]);
            for (s = 0; s < SCDA_INCL_SLOTS; s++)
                if (tab[2 * s] && tab[2 * s + 1] * 20 >= walks[k])
                    scda_print_rip("[SAMPLE-INCL]  ", 100.0 * (double)tab[2 * s + 1] / walks[k],
                                   tab[2 * s], base);
        }
        CloseHandle(th[k]);
    }
    for (unsigned c = 0; c < 256 && wait_site[c][3]; c++)
        if (wait_site[c][3] >= 3)
            fprintf(stderr, "[SAMPLE-WAIT] thread %lu: %llu waits from exe+0x%llX <- exe+0x%llX\n",
                    (unsigned long)tid[wait_site[c][0]],
                    (unsigned long long)wait_site[c][3],
                    (unsigned long long)(wait_site[c][1] - base),
                    (unsigned long long)(wait_site[c][2] ? wait_site[c][2] - base : 0));
    for (unsigned c = 0; c < 256 && wait_site[c][3]; c++)
        if (wait_site[c][3] >= 3)
            fprintf(stderr, "[SAMPLE-WAIT]   deeper (last walk): exe+0x%llX exe+0x%llX "
                    "exe+0x%llX exe+0x%llX\n",
                    (unsigned long long)(wait_site[c][4] ? wait_site[c][4] - base : 0),
                    (unsigned long long)(wait_site[c][5] ? wait_site[c][5] - base : 0),
                    (unsigned long long)(wait_site[c][6] ? wait_site[c][6] - base : 0),
                    (unsigned long long)(wait_site[c][7] ? wait_site[c][7] - base : 0));
    for (unsigned c = 0; c < 64 && crt_caller[c][0]; c++)
        if (crt_caller[c][1] >= 2)
            fprintf(stderr, "[SAMPLE-CRT] %llu walks with a C-runtime leaf called from exe+0x%llX\n",
                    (unsigned long long)crt_caller[c][1],
                    (unsigned long long)(crt_caller[c][0] - base));
    fflush(stderr);
    free(rips);
    free(incl);
    return 0;
}

/* F9 in the game window (scda_input.c, docs Update 489): sample every
 * thread for 10 s from now, so a slow spot can be measured where the
 * player stands. At most one window every 15 s. */
void scda_sample_all_now(void)
{
    static ULONGLONG last;
    static char spec[] = "0,10";
    ULONGLONG now = GetTickCount64();
    if (last && now - last < 15000)
        return;
    last = now;
    fprintf(stderr, "[SAMPLE-ALL] F9: sampling all threads for 10 s\n");
    CloseHandle(CreateThread(NULL, 0, scda_sampler_all_thread, spec, 0, NULL));
}

static void scda_start_sampler(void)
{
    const char *spec = getenv("RECOMP_SAMPLE_MAIN");
    const char *all = getenv("RECOMP_SAMPLE_ALL");
    if (all && *all)
        CloseHandle(CreateThread(NULL, 0, scda_sampler_all_thread, (LPVOID)all, 0, NULL));
    if (!spec || !*spec)
        return;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                    &g_scda_main_thread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    CloseHandle(CreateThread(NULL, 0, scda_sampler_thread, (LPVOID)spec, 0, NULL));
}

/* Name of this process's parent, for the second-instance note below. */
static void scda_parent_process(DWORD *pid_out, char *name, size_t name_size)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe;
    DWORD self = GetCurrentProcessId(), parent = 0;

    *pid_out = 0;
    strcpy_s(name, name_size, "?");
    if (snap == INVALID_HANDLE_VALUE)
        return;
    pe.dwSize = sizeof pe;
    for (BOOL ok = Process32First(snap, &pe); ok; ok = Process32Next(snap, &pe))
        if (pe.th32ProcessID == self)
            parent = pe.th32ParentProcessID;
    pe.dwSize = sizeof pe;
    for (BOOL ok = Process32First(snap, &pe); ok && parent; ok = Process32Next(snap, &pe))
        if (pe.th32ProcessID == parent)
            strcpy_s(name, name_size, pe.szExeFile);
    CloseHandle(snap);
    *pid_out = parent;
}

/* One game process at a time (docs Update 467). Two instances both read the
 * controller and keyboard, share the log files and, pinned to the same
 * core, slow each other down -- a run on 2026-09-30 had an unexplained fresh
 * second instance next to the relaunched in-level one. The named mutex only
 * marks that an instance is alive (any handle keeps the name in use, so no
 * thread ownership is involved). A relaunch child (launch_data.bin present)
 * waits for the exiting parent to let go; any other start while an instance
 * runs is refused, and who started it goes to scda_second_instance.log. */
static HANDLE s_instance_lock;
static FILE *s_log_file;
static char s_log_path[MAX_PATH];

static void scda_single_instance(int relaunch)
{
    static const char name[] = "Local\\scda_recomp_instance";
    HANDLE m = CreateMutexA(NULL, FALSE, name);
    int waited = 0;

    while (m && GetLastError() == ERROR_ALREADY_EXISTS && relaunch && waited < 15000) {
        CloseHandle(m);
        Sleep(100);
        waited += 100;
        m = CreateMutexA(NULL, FALSE, name);
    }
    if (!m || GetLastError() != ERROR_ALREADY_EXISTS) {
        s_instance_lock = m;   /* kept open until exit or a relaunch */
        return;
    }
    if (relaunch)
        return;   /* parent still alive after 15 s -- start anyway */
    {
        char path[MAX_PATH], parent_name[MAX_PATH];
        DWORD parent;
        SYSTEMTIME t;
        FILE *f = NULL;

        scda_parent_process(&parent, parent_name, sizeof parent_name);
        GetLocalTime(&t);
        scda_path_in_exe_dir(path, sizeof(path), "scda_second_instance.log");
        if (fopen_s(&f, path, "a") == 0 && f) {
            fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u refused second instance pid=%lu "
                    "parent pid=%lu (%s) cmdline=%s\n",
                    t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                    (unsigned long)GetCurrentProcessId(), (unsigned long)parent,
                    parent_name, GetCommandLineA());
            fclose(f);
        }
    }
    MessageBoxA(NULL, "SCDA is already running in another window.\n"
                "This second start is closed.", "SCDA Recomp",
                MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
    ExitProcess(0);
}

/* Relaunch with window handover (docs Update 502): this instance lives on,
 * showing its last frame, until the new one has drawn. Before the new one
 * starts: silence the audio (the console's reboot stops it), drop the
 * single-instance lock so the new one does not wait for it, and move this
 * log to scda_runtime_prelaunch.log so the new one can create its own. */
void xa2_mute(void);
void xbox_SetRelaunchHostHook(void (*fn)(void));

static void scda_before_relaunch(void)
{
    char prev[MAX_PATH];
    FILE *f = NULL;
    int nul;

    xa2_mute();
    if (s_instance_lock) {
        CloseHandle(s_instance_lock);
        s_instance_lock = NULL;
    }
    if (!s_log_file)
        return;
    fflush(stdout);
    fflush(stderr);
    nul = _open("NUL", _O_WRONLY);
    if (nul >= 0) {
        _dup2(nul, _fileno(stdout));
        _dup2(nul, _fileno(stderr));
        _close(nul);
    }
    fclose(s_log_file);
    s_log_file = NULL;
    scda_path_in_exe_dir(prev, sizeof(prev), "scda_runtime_prelaunch.log");
    MoveFileExA(s_log_path, prev, MOVEFILE_REPLACE_EXISTING);
    f = _fsopen(prev, "a", _SH_DENYNO);
    if (f) {
        _dup2(_fileno(f), _fileno(stdout));
        _dup2(_fileno(f), _fileno(stderr));
        setvbuf(f, NULL, _IONBF, 0);
        s_log_file = f;
    }
}

/* Release layout (scda_layout.c): game in gamefiles\, everything written in
 * user\. Resolved before the log and launch_data.bin are touched, because
 * both live in user\ then. */
static int s_release_layout;
static char s_release_game_dir[MAX_PATH];

static void scda_init_layout(void)
{
    char user_dir[MAX_PATH];

    if (!scda_layout_resolve(g_scda_exe_dir, s_release_game_dir,
                             sizeof(s_release_game_dir), user_dir,
                             sizeof(user_dir)))
        return;
    s_release_layout = 1;
    strcpy_s(g_scda_data_dir, sizeof(g_scda_data_dir), user_dir);
    /* relative writes (launch pages, diagnostics) land in user\ too */
    SetCurrentDirectoryA(user_dir);
    _putenv_s("RECOMP_DATA_DIR", user_dir);
}

static void scda_init_diagnostics(void)
{
    scda_init_exe_dir();
    scda_init_layout();
    {
        char launch_path[MAX_PATH];
        scda_path_in_exe_dir(launch_path, sizeof(launch_path), "launch_data.bin");
        scda_single_instance(GetFileAttributesA(launch_path) != INVALID_FILE_ATTRIBUTES);
        /* Unattended tests: the relaunched in-level instance inherits the
         * environment, so it would replay the menu presses of
         * RECOMP_INPUT_SCRIPT (START opens the pause menu, ...). When
         * RECOMP_INPUT_SCRIPT_RELAUNCH is set, the relaunch uses it instead
         * (empty = no presses). */
        if (GetFileAttributesA(launch_path) != INVALID_FILE_ATTRIBUTES) {
            const char *rs = getenv("RECOMP_INPUT_SCRIPT_RELAUNCH");
            if (rs) {
                _putenv_s("RECOMP_INPUT_SCRIPT", *rs ? rs : "none");
                /* A menu/profile relaunch may precede the mission relaunch.
                 * For unattended Bink tests, press through only that first
                 * menu child, then leave the mission movie untouched. */
                if (getenv("RECOMP_INPUT_SCRIPT_RELAUNCH_ONCE"))
                    _putenv_s("RECOMP_INPUT_SCRIPT_RELAUNCH", "none");
            }
        }
    }
    scda_path_in_exe_dir(s_log_path, sizeof(s_log_path), "scda_runtime.log");
    /* A relaunch (the title called XLaunchNewImage, see
     * bridge_relaunch_same_title) starts while the previous instance may
     * still hold the log open. Keep its log as scda_runtime_prelaunch.log
     * and wait briefly for it to go away. */
    {
        char launch_path[MAX_PATH], prev_path[MAX_PATH];
        int tries;
        scda_path_in_exe_dir(launch_path, sizeof(launch_path), "launch_data.bin");
        if (GetFileAttributesA(launch_path) != INVALID_FILE_ATTRIBUTES) {
            scda_path_in_exe_dir(prev_path, sizeof(prev_path), "scda_runtime_prelaunch.log");
            for (tries = 0; tries < 100; tries++) {
                if (MoveFileExA(s_log_path, prev_path, MOVEFILE_REPLACE_EXISTING) ||
                    GetLastError() == ERROR_FILE_NOT_FOUND)
                    break;
                Sleep(50);
            }
        }
    }
    /* _SH_DENYNO: readable while the game runs (fopen_s locks it). */
    s_log_file = _fsopen(s_log_path, "w", _SH_DENYNO);
    if (s_log_file) {
        _dup2(_fileno(s_log_file), _fileno(stdout));
        _dup2(_fileno(s_log_file), _fileno(stderr));
        setvbuf(s_log_file, NULL, _IONBF, 0);
    }
    SetUnhandledExceptionFilter(scda_write_crash_dump);
    _set_invalid_parameter_handler(scda_invalid_parameter);
    signal(SIGABRT, scda_on_sigabrt);
    xbox_SetRelaunchHostHook(scda_before_relaunch);
    scda_start_sampler();
}
#else /* !_WIN32 */

/* Linux start-up (ARM port, docs Update 677): the same release layout,
 * options file and log as on Windows, on top of the Win32 shim, where paths
 * are Windows-style ("Z:\home\..."). The crash filter logs the signal and the
 * host backtrace; there is no minidump and no sampling profiler. */
#include <execinfo.h>
#include <unistd.h>

static char g_scda_exe_dir[MAX_PATH];
static char g_scda_data_dir[MAX_PATH];
static int s_release_layout;
static char s_release_game_dir[MAX_PATH];
static FILE *s_log_file;

static void scda_path_in_exe_dir(char *out, size_t out_size, const char *name)
{
    snprintf(out, out_size, "%s\\%s",
             g_scda_data_dir[0] ? g_scda_data_dir : g_scda_exe_dir, name);
}

static LONG WINAPI scda_posix_crash(EXCEPTION_POINTERS *exception)
{
    static volatile LONG reported;
    void *frames[48];
    int n;

    if (InterlockedCompareExchange(&reported, 1, 0) != 0)
        return EXCEPTION_CONTINUE_SEARCH;
    fprintf(stderr, "[SCDA-CRASH] exception=0x%08lX address=%p tid=%lu "
            "host_pc=%p guest_esp=0x%08X",
            (unsigned long)exception->ExceptionRecord->ExceptionCode,
            exception->ExceptionRecord->ExceptionAddress,
            (unsigned long)GetCurrentThreadId(),
            (void *)(uintptr_t)XBOX_CONTEXT_PC(exception->ContextRecord),
            (unsigned)g_esp);
    if (exception->ExceptionRecord->NumberParameters >= 2)
        fprintf(stderr, " access=%llu fault=%p",
                (unsigned long long)exception->ExceptionRecord->ExceptionInformation[0],
                (void *)(uintptr_t)exception->ExceptionRecord->ExceptionInformation[1]);
    fputc('\n', stderr);
    n = backtrace(frames, 48);
    backtrace_symbols_fd(frames, n, fileno(stderr));
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void scda_posix_init(void)
{
    char *sep;
    DWORD len = GetModuleFileNameA(NULL, g_scda_exe_dir, MAX_PATH);
    if (!len || len >= MAX_PATH)
        snprintf(g_scda_exe_dir, sizeof(g_scda_exe_dir), ".");
    else if ((sep = strrchr(g_scda_exe_dir, '\\')) != NULL)
        *sep = '\0';

    {
        char user_dir[MAX_PATH];
        if (scda_layout_resolve(g_scda_exe_dir, s_release_game_dir,
                                sizeof(s_release_game_dir), user_dir,
                                sizeof(user_dir))) {
            s_release_layout = 1;
            snprintf(g_scda_data_dir, sizeof(g_scda_data_dir), "%s", user_dir);
            SetCurrentDirectoryA(user_dir);
            setenv("RECOMP_DATA_DIR", user_dir, 1);
        }
    }
    /* the log next to the data, as on Windows; RECOMP_LOG_STDERR=1 keeps
     * the terminal */
    if (!getenv("RECOMP_LOG_STDERR") || getenv("RECOMP_LOG_STDERR")[0] == '0') {
        char log_path[MAX_PATH];
        scda_path_in_exe_dir(log_path, sizeof(log_path), "scda_runtime.log");
        s_log_file = fopen(log_path, "w");
        if (s_log_file) {
            dup2(fileno(s_log_file), fileno(stdout));
            dup2(fileno(s_log_file), fileno(stderr));
            setvbuf(s_log_file, NULL, _IONBF, 0);
        }
    }
    SetUnhandledExceptionFilter(scda_posix_crash);
}

/* the all-threads sampler is Windows-only (thread contexts + unwinding) */
void scda_sample_all_now(void) {}
#endif

static int load_file(const char *path, void **data_out, size_t *size_out)
{
    FILE *file = NULL;
    long length;
    void *data;

    if (fopen_s(&file, path, "rb") != 0 || !file)
        return 0;
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) <= 0) {
        fclose(file);
        return 0;
    }
    rewind(file);
    data = malloc((size_t)length);
    if (!data || fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return 0;
    }
    fclose(file);
    *data_out = data;
    *size_out = (size_t)length;
    return 1;
}

/* Hybrid-graphics laptops (docs Update 674): ask the NVIDIA Optimus and AMD
 * PowerXpress drivers for the discrete GPU. Without this the OpenGL window
 * landed on the integrated GPU as soon as the EXE was moved and lost its
 * per-path GPU preference, while Vulkan still picked the discrete card. */
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

int main(void)
{
    void *xbe_data = NULL;
    size_t xbe_size = 0;
    const char *xbe_path = SCDA_XBE_PATH;
    const char *game_dir = SCDA_GAME_DIR;
    const char *save_dir = "projects\\scda\\saves";

#ifdef _WIN32
    char adjacent_xbe[MAX_PATH];
    char adjacent_saves[MAX_PATH];

    /* Per-monitor DPI aware (docs Update 669): without it, Windows display
     * scaling above 100% gave the game a smaller virtual screen and
     * stretched the finished picture itself -- blurry and blocky, worst in
     * fullscreen. Before any window exists. */
    {
        typedef BOOL (WINAPI *dpi_ctx_fn)(HANDLE);
        dpi_ctx_fn set_ctx = (dpi_ctx_fn)(void *)GetProcAddress(
            GetModuleHandleA("user32.dll"), "SetProcessDpiAwarenessContext");
        if (!set_ctx || !set_ctx((HANDLE)(intptr_t)-4))   /* PER_MONITOR_AWARE_V2 */
            SetProcessDPIAware();
    }

    /* scda_options.txt next to the EXE (docs Update 614): the player's
     * settings become RECOMP_* variables before anything reads them. */
    scda_options_load();
    /* 2026-09-11: the original Xbox has exactly one physical CPU core. A
     * title's own worker threads (SCDA runs at least two concurrent Bink
     * decode workers for its whole lifetime) can be written assuming
     * operations the console's single core serializes by never actually
     * interleaving them at the instruction level a preemptive scheduler on
     * *real* separate cores absolutely can. On this host, those two
     * threads run on genuinely different cores by default, so a data race
     * the game's own code never has to protect against on hardware (because
     * hardware never runs it truly in parallel) can manifest here as
     * spurious cross-thread memory corruption -- caught live as
     * sub_004B89E0/sub_004B8FC0 getting back a corrupted saved register
     * that turned out to be another concurrently-running Bink stream
     * object's own pointer (see docs/SCDA_WORKLOG.md). Pinning every thread
     * in this process to one core is not a symptomatic workaround, it is
     * matching the one piece of real hardware topology this recomp had not
     * yet reproduced. Failure is silently ignored: worst case is running
     * exactly as before on whatever cores the OS picks.
     *
     * 2026-09-26 (docs Update 345): pin the guest threads, not the process.
     * Pinning the process also put the emulated devices (NV2A handshake
     * acknowledgement, APU, display) on that one core, and a guest thread
     * spinning on a GPU handshake bit then held the core for a full
     * scheduler quantum before the device thread could clear it -- ~100 ms
     * frames, Bink audio starvation. The guest threads created later pin
     * themselves in bridge_thread_main / xbox_thread_wrapper. */
    xbox_PinGuestThread();
    scda_init_diagnostics();
    if (s_release_layout) {
        static char release_xbe[MAX_PATH], release_saves[MAX_PATH],
                    user_utf8[MAX_PATH];
        snprintf(release_xbe, sizeof(release_xbe), "%s\\default.xbe",
                 s_release_game_dir);
        snprintf(release_saves, sizeof(release_saves), "%s\\hdd",
                 g_scda_data_dir);
        CreateDirectoryA(release_saves, NULL);
        xbe_path = release_xbe;
        game_dir = s_release_game_dir;
        save_dir = release_saves;
        /* UDATA, TDATA and CACHE (the console's E: drive) directly in user\ */
        scda_acp_to_utf8(g_scda_data_dir, user_utf8, sizeof(user_utf8));
        xbox_path_set_partition1_dir(user_utf8);
    }
    snprintf(adjacent_xbe, sizeof(adjacent_xbe), "%s\\default.xbe",
             g_scda_exe_dir);
    if (!s_release_layout &&
        GetFileAttributesA(adjacent_xbe) != INVALID_FILE_ATTRIBUTES) {
        xbe_path = adjacent_xbe;
        game_dir = g_scda_exe_dir;
        scda_path_in_exe_dir(adjacent_saves, sizeof(adjacent_saves),
                             "recomp_saves");
        CreateDirectoryA(adjacent_saves, NULL);
        save_dir = adjacent_saves;
        SetCurrentDirectoryA(g_scda_exe_dir);
    }
    /* Diagnostic only: compare cold/warm title-cache behavior without
     * moving or clearing the user's adjacent recomp_saves directory. */
    {
        const char *isolated_save_dir = getenv("RECOMP_SCDA_TEST_SAVE_DIR");
        if (isolated_save_dir && *isolated_save_dir) {
            if (!((isolated_save_dir[0] >= 'A' && isolated_save_dir[0] <= 'Z') ||
                  (isolated_save_dir[0] >= 'a' && isolated_save_dir[0] <= 'z')) ||
                isolated_save_dir[1] != ':' ||
                (isolated_save_dir[2] != '\\' && isolated_save_dir[2] != '/')) {
                fprintf(stderr, "RECOMP_SCDA_TEST_SAVE_DIR must be an absolute drive path\n");
                return 2;
            }
            save_dir = isolated_save_dir;
        }
    }
#else
    scda_options_load();
    xbox_PinGuestThread();
    scda_posix_init();
    if (s_release_layout) {
        static char release_xbe[MAX_PATH], release_saves[MAX_PATH];
        snprintf(release_xbe, sizeof(release_xbe), "%s\\default.xbe",
                 s_release_game_dir);
        snprintf(release_saves, sizeof(release_saves), "%s\\hdd",
                 g_scda_data_dir);
        CreateDirectoryA(release_saves, NULL);
        xbe_path = release_xbe;
        game_dir = s_release_game_dir;
        save_dir = release_saves;
        xbox_path_set_partition1_dir(g_scda_data_dir);
    }
#endif

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    puts("SCDA static recompilation bootstrap");
    printf("Executable directory: %s\n",
           g_scda_exe_dir
    );
    printf("XBE path: %s\nGame directory: %s\nSave directory: %s\n",
           xbe_path, game_dir, save_dir);

    if (!load_file(xbe_path, &xbe_data, &xbe_size)) {
        fprintf(stderr, "Could not load %s.\n", xbe_path);
        return 1;
    }
    /* SCDA is a retail title (no devkit RAM upgrade); XBOX_TOTAL_RAM's
     * default already matches this, but set it explicitly and early,
     * before any static-initializer ordering ambiguity could matter, as
     * a diagnostic experiment for a hang under investigation this
     * session (see docs/SCDA_WORKLOG.md, 2026-09-01 entries). */
    xbox_SetTotalRam(64u * 1024u * 1024u);
    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        fputs("Xbox memory layout initialization failed.\n", stderr);
        free(xbe_data);
        return 2;
    }

    /* PFIFO bring-up (minimal, consume-only): sub_004A1490's pushbuffer
     * space-reservation wait reads *MEM32(device+0x30) ("get") and compares
     * it against MEM32(device+0x2C) ("put") in a tight loop that spins
     * forever once the pushbuffer fills, because nothing ever advances the
     * get-pointer shadow it polls -- see docs/SCDA_WORKLOG.md, 2026-09-05
     * continued 46. xbox_Nv2aMirrorFence is this project's existing generic
     * bring-up mechanism for exactly that shape (a per-channel put counter
     * plus a pointer to its own get shadow in guest memory): every tick it
     * re-reads both fields live and copies put into *get, i.e. "treat
     * everything submitted so far as consumed". That is not real command
     * processing -- see src/nv2a/nv2a_core.c's own PFIFO_CACHE/USER
     * STUB_ENTRYs -- just enough that this device's own driver code stops
     * waiting on a GPU-progress signal our emulation never produces
     * otherwise. The device pointer (0x4AB668) and the +0x2C/+0x30 field
     * offsets come straight from sub_004A1490's disassembly and are this
     * binary's own D3D globals, not a value baked into the emulator. */
    xbox_Nv2aMirrorFence(0x004AB668u, 0x2Cu, 0x30u);

    xbox_kernel_init();
#ifdef _WIN32
    {
        static char game_utf8[MAX_PATH], save_utf8[MAX_PATH];
        scda_acp_to_utf8(game_dir, game_utf8, sizeof(game_utf8));
        scda_acp_to_utf8(save_dir, save_utf8, sizeof(save_utf8));
        xbox_path_init(game_utf8, save_utf8);
    }
#else
    xbox_path_init(game_dir, save_dir);
#endif
    xbox_kernel_bridge_init();
    g_esp = XBOX_STACK_TOP;

    /* Flat dispatch table (parity with templates/new-game, docs Update 644):
     * SCDA never built it, so every indirect call that is not a manual
     * override binary-searched the 32k-entry function table -- recomp_lookup
     * showed up in the game-thread profile. Optional: on failure the search
     * still works. */
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; "
                        "indirect calls will use the binary search\n");
    else
        fprintf(stderr, "[BOOT] flat dispatch table built (%zu KB)\n",
                recomp_dispatch_flat_bytes() / 1024);

    /* Keep the project bootstrap in parity with templates/new-game: this must
     * run on the thread that owns the guest TLS registers.  It is inert unless
     * RECOMP_WATCHDOG_SECS is set and lets renderer bring-up identify a CPU
     * polling loop that otherwise makes no kernel or MMIO calls. */
    xbox_WatchdogStart();

    /* 60 FPS policy needs the launch page's single/co-op mode before the
     * title reads it (recomp_manual.c, docs Update 649). */
    {
        extern void scda_fps_init(void);
        scda_fps_init();
    }

    printf("Calling SCDA entry point 0x%08X\n", SCDA_ENTRY_POINT);
    xbe_entry_point();

    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe_data);
    return 0;
}
