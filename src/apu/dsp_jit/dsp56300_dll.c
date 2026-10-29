/*
 * Runtime binding of the dsp56300 C API (mborgerson/dsp56300 v1.0.0, MIT,
 * see LICENSE.dsp56300) to dsp56300.dll.
 *
 * The library ships as a Rust static archive for the windows-gnu target,
 * which MSVC cannot link directly. build_dsp56300_dll.ps1 wraps that archive
 * into a plain DLL with MinGW; this file defines the API functions dsp.c
 * calls as forwarders into it. Loading is explicit (dsp56300_dll_load), so
 * the executable still starts without the DLL -- surround audio is then
 * unavailable and the stereo interpreter path is used as before.
 *
 * x64 has a single C calling convention, and the API passes only plain
 * C structs with fixed-width fields, so the MinGW/MSVC boundary is safe.
 */
#include <windows.h>
#include <stdio.h>

#include "dsp56300.h"

/* X(ret, name, params, args, RET): RET is `return` for a value-returning
 * entry point and empty for a void one. */
#define DSP56300_API(X) \
    X(Dsp56300Jit *, create, (const Dsp56300CreateInfo *info), (info), return) \
    X(void, destroy, (Dsp56300Jit *dsp), (dsp), ) \
    X(void, dump_profile, (Dsp56300Jit *dsp, const char *path), (dsp, path), ) \
    X(void, reset, (Dsp56300Jit *dsp), (dsp), ) \
    X(void, step, (Dsp56300Jit *dsp), (dsp), ) \
    X(void, run, (Dsp56300Jit *dsp, int32_t cycles), (dsp, cycles), ) \
    X(bool, halt_requested, (const Dsp56300Jit *dsp), (dsp), return) \
    X(void, set_halt_requested, (Dsp56300Jit *dsp, bool halt), (dsp, halt), ) \
    X(uint32_t, cycle_count, (const Dsp56300Jit *dsp), (dsp), return) \
    X(void, set_cycle_count, (Dsp56300Jit *dsp, uint32_t count), (dsp, count), ) \
    X(void, invalidate_cache, (Dsp56300Jit *dsp), (dsp), ) \
    X(void, get_jit_stats, (const Dsp56300Jit *dsp, Dsp56300JitStats *out), \
      (dsp, out), ) \
    X(uint32_t, read_memory, (const Dsp56300Jit *dsp, Dsp56300MemSpace space, \
      uint32_t addr), (dsp, space, addr), return) \
    X(void, write_memory, (Dsp56300Jit *dsp, Dsp56300MemSpace space, \
      uint32_t addr, uint32_t value), (dsp, space, addr, value), ) \
    X(void, read_memory_run, (const Dsp56300Jit *dsp, Dsp56300MemSpace space, \
      uint32_t addr, uint32_t *out, uint32_t count), \
      (dsp, space, addr, out, count), ) \
    X(void, write_memory_run, (Dsp56300Jit *dsp, Dsp56300MemSpace space, \
      uint32_t addr, const uint32_t *vals, uint32_t count), \
      (dsp, space, addr, vals, count), ) \
    X(void, get_state, (const Dsp56300Jit *dsp, Dsp56300State *out), \
      (dsp, out), ) \
    X(void, set_state, (Dsp56300Jit *dsp, const Dsp56300State *src), \
      (dsp, src), )

#define DECLARE_PTR(ret, name, params, args, RET) \
    static ret (*p_##name) params;
DSP56300_API(DECLARE_PTR)

#define DEFINE_FWD(ret, name, params, args, RET) \
    ret dsp56300_##name params { RET p_##name args; }
DSP56300_API(DEFINE_FWD)

/* Loads dsp56300.dll next to the executable. Returns 1 when every entry
 * point resolved; the result is cached. */
int dsp56300_dll_load(void)
{
    static int state = -1;
    HMODULE mod;
    wchar_t path[MAX_PATH];
    DWORD n;

    if (state >= 0)
        return state;
    state = 0;
#if !defined(_WIN32)
    /* no Linux build of the JIT core yet (ARM port, docs Update 677): the
     * surround path stays off and stereo uses the interpreter-free mix */
    (void)mod; (void)path; (void)n;
    fprintf(stderr, "[APU-SURROUND] no DSP56300 JIT core on this host\n");
    return 0;
#endif

    /* Only the executable's own directory, never the search path. */
    n = GetModuleFileNameW(NULL, path, MAX_PATH);
    while (n && path[n - 1] != L'\\')
        n--;
    if (!n || n + 16 >= MAX_PATH)
        return 0;
    wcscpy_s(path + n, MAX_PATH - n, L"dsp56300.dll");
    mod = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!mod) {
        fprintf(stderr, "[APU-SURROUND] dsp56300.dll not found next to the "
                        "executable (error %lu)\n", GetLastError());
        return 0;
    }

#define RESOLVE(ret, name, params, args, RET) \
    p_##name = (ret (*) params)(void *)GetProcAddress(mod, "dsp56300_" #name); \
    if (!p_##name) { \
        fprintf(stderr, "[APU-SURROUND] dsp56300.dll lacks dsp56300_%s\n", #name); \
        return 0; \
    }
    DSP56300_API(RESOLVE)
#undef RESOLVE

    state = 1;
    fprintf(stderr, "[APU-SURROUND] DSP56300 JIT core loaded (dsp56300.dll)\n");
    return 1;
}
