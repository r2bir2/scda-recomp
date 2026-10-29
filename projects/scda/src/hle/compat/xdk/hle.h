// HLE function macros of SCDA-XBERecomp (hle/tgp), re-targeted: instead of
// defining the recompiled guest function directly, each host implementation
// registers itself by name; hle_d3d_wrappers.c (generated) routes the guest
// call to it through hle_d3d_dispatch() when RECOMP_HLE_D3D=1, otherwise the
// recompiled original runs. See agent_handoff/HLE_RENDERER_PLAN.md.
#pragma once
#include <cstring>

#include "core/log.h"
#include "image_symbols.h"
#include "kernel/kernel.h"

#define X86_CAT_(a, b) a##b
#define X86_CAT(a, b) X86_CAT_(a, b)
#define SYM(name) X86_CAT(0x, SYM_##name)  // address

namespace hle {

using Fn = uint32_t (*)(X86Context& ctx, const ::kernel::Args& a);

enum Kind {
    kReplace,  // returns to the guest caller itself: eax, stack cleanup
    kBefore,   // runs, then the recompiled original runs as well
};

struct Registration {
    Registration(const char* name, Fn fn, int stack_args, Kind kind);
};

}  // namespace hle

#define HLE_REGISTER_(name, nargs, kind)                                  \
    static uint32_t hle_##name(X86Context& ctx, const ::kernel::Args& a); \
    static ::hle::Registration hle_reg_##name(#name, hle_##name, nargs, kind);

// Host implementation of a guest function (stdcall: nargs stack arguments)
#define HLE_STDCALL(name, nargs)                     \
    HLE_REGISTER_(name, nargs, ::hle::kReplace)      \
    static uint32_t hle_##name([[maybe_unused]] X86Context& ctx, [[maybe_unused]] const ::kernel::Args& a)

// __fastcall: first two arguments in ecx and edx, stack_args on the stack
#define HLE_FASTCALL(name, stack_args) HLE_STDCALL(name, stack_args)

// __cdecl: the caller pops the arguments
#define HLE_CDECL(name) HLE_STDCALL(name, 0)

// work done before the recompiled original, which then runs unchanged
// (SCDA-XBERecomp writes these as X86_FUNC overrides calling GUEST_ORIG)
#define HLE_BEFORE(name)                             \
    HLE_REGISTER_(name, 0, ::hle::kBefore)           \
    static uint32_t hle_##name([[maybe_unused]] X86Context& ctx, [[maybe_unused]] const ::kernel::Args& a)

// float arguments passed on the stack
inline float ArgF(const ::kernel::Args& a, int i) {
    uint32_t v = a[i];
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
inline float GF(uint32_t va) {
    float f;
    std::memcpy(&f, ::kernel::HostPtr(va), 4);
    return f;
}
