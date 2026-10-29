// Runtime glue for the SCDA-XBERecomp renderer sources under hle/tgp: the
// kernel:: and mem:: helpers they call, mapped onto this recompiler's runtime
// (guest memory at g_xbox_mem_offset, guest registers g_eax..g_esp as
// thread-local globals, functions looked up by address). See
// agent_handoff/HLE_RENDERER_PLAN.md.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>

#if defined(_MSC_VER)
#define HLE_GUEST_TLS __declspec(thread)
#else
#define HLE_GUEST_TLS __thread
#endif

extern "C" {
extern ptrdiff_t g_xbox_mem_offset;
extern HLE_GUEST_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
uint32_t xbox_ContiguousAlloc(uint32_t size, uint32_t alignment);
}

// the guest registers the host implementations read and set
struct X86Context {
    uint32_t &eax, &ecx, &edx, &esp;
};

namespace kernel {

inline uint8_t* Base() { return reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(g_xbox_mem_offset)); }
inline void* HostPtr(uint32_t va) { return Base() + va; }
template <typename T>
inline T& G(uint32_t va) {
    return *reinterpret_cast<T*>(Base() + va);
}
inline uint32_t R32(uint32_t va) {
    uint32_t v;
    std::memcpy(&v, Base() + va, 4);
    return v;
}
inline void W32(uint32_t va, uint32_t v) { std::memcpy(Base() + va, &v, 4); }
inline uint16_t R16(uint32_t va) {
    uint16_t v;
    std::memcpy(&v, Base() + va, 2);
    return v;
}
inline void W16(uint32_t va, uint16_t v) { std::memcpy(Base() + va, &v, 2); }
inline uint8_t R8(uint32_t va) { return Base()[va]; }
inline void W8(uint32_t va, uint8_t v) { Base()[va] = v; }
inline uint64_t R64(uint32_t va) {
    uint64_t v;
    std::memcpy(&v, Base() + va, 8);
    return v;
}
inline void W64(uint32_t va, uint64_t v) { std::memcpy(Base() + va, &v, 8); }

// stdcall/fastcall stack arguments of the guest call being replaced
struct Args {
    X86Context& ctx;
    uint32_t operator[](int i) const { return R32(ctx.esp + 4 + 4 * uint32_t(i)); }
};
inline void Return(X86Context& ctx, uint32_t value, int stack_args) {
    ctx.eax = value;
    ctx.esp += 4 + 4 * uint32_t(stack_args);
}

X86Context& CurrentContext();
// runs a recompiled guest function with stdcall/cdecl arguments; esp is
// restored afterwards, so either convention is fine
uint32_t CallGuest(X86Context& ctx, uint32_t fn, std::initializer_list<uint32_t> args);

// the console's persisted display across a quick reboot: not kept here
inline void SavePersistedDisplay(uint32_t, uint32_t, uint32_t, const void*, uint32_t) {}
inline void SetSavedDataAddress(uint32_t) {}

}  // namespace kernel

namespace mem {
uint32_t AllocContiguous(uint32_t size, uint32_t lowest, uint32_t highest, uint32_t align);
uint32_t AllocSystem(uint32_t size, uint32_t align = 16);
void FreeSystem(uint32_t va);
// physical == virtual in this runtime (kernel bridge MmGetPhysicalAddress)
inline uint32_t PhysicalFromVirtual(uint32_t va) { return va; }
inline uint32_t VirtualFromPhysical(uint32_t pa) { return pa; }
}  // namespace mem
