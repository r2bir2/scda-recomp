// HLE renderer glue (agent_handoff/HLE_RENDERER_PLAN.md): routes the generated
// D3D wrappers (hle_d3d_wrappers.c) to the host implementations in
// hle/tgp/xdk/d3d8.cpp (from SCDA-XBERecomp, MIT, (c) 2026 TGP482,
// Joshhhuaaa), and provides the runtime services those sources expect.
//
// Milestone M2: a null backend -- the title's D3D calls are handled, nothing
// is drawn. RECOMP_HLE_D3D=1 switches the HLE path on (read once).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "app/app.h"
#include "gpu/backend.h"
#include "kernel/kernel.h"
#include "xdk/hle.h"

extern "C" {
#include "hle_d3d_ids.h"
}

// ---- guest calls and memory ------------------------------------------------

X86Context& kernel::CurrentContext() {
    static HLE_GUEST_TLS X86Context* ctx;
    if (!ctx) ctx = new X86Context{g_eax, g_ecx, g_edx, g_esp};
    return *ctx;
}

uint32_t kernel::CallGuest(X86Context& ctx, uint32_t fn, std::initializer_list<uint32_t> args) {
    (void)ctx;
    recomp_func_t f = recomp_lookup(fn);
    if (!f) {
        std::fprintf(stderr, "[HLE] E CallGuest: no function at 0x%08X\n", fn);
        return 0;
    }
    uint32_t saved = g_esp;
    std::vector<uint32_t> v(args);
    for (size_t i = v.size(); i-- > 0;) {
        g_esp -= 4;
        W32(g_esp, v[i]);
    }
    g_esp -= 4;
    W32(g_esp, 0xFEEDF00Du);  // return address: the callee only pops it
    f();
    g_esp = saved;
    return g_eax;
}

namespace {
// small guest allocations (shader handle records, scratch headers): a bump
// allocator in contiguous memory; frees are not reused
std::mutex g_sys_lock;
uint32_t g_sys_base = 0, g_sys_pos = 0, g_sys_end = 0;
}  // namespace

uint32_t mem::AllocContiguous(uint32_t size, uint32_t lowest, uint32_t highest, uint32_t align) {
    (void)lowest;
    (void)highest;
    uint32_t va = xbox_ContiguousAlloc(size, align < 4096 ? 4096 : align);
    if (va) std::memset(kernel::HostPtr(va), 0, size);
    return va;
}

uint32_t mem::AllocSystem(uint32_t size, uint32_t align) {
    std::lock_guard<std::mutex> lock(g_sys_lock);
    if (align < 16) align = 16;
    uint32_t pos = (g_sys_pos + align - 1) & ~(align - 1);
    if (!g_sys_base || pos + size > g_sys_end) {
        uint32_t chunk = size + align > 0x10000 ? size + align : 0x10000;
        g_sys_base = mem::AllocContiguous(chunk, 0, 0xFFFFFFFF, 4096);
        g_sys_end = g_sys_base + chunk;
        pos = (g_sys_base + align - 1) & ~(align - 1);
    }
    g_sys_pos = pos + size;
    std::memset(kernel::HostPtr(pos), 0, size);
    return pos;
}

void mem::FreeSystem(uint32_t) {}

// ---- registration and dispatch ----------------------------------------------

namespace {
struct Entry {
    hle::Fn fn = nullptr;
    int stack_args = 0;
    hle::Kind kind = hle::kReplace;
};
std::unordered_map<std::string, Entry>& Registry() {
    static std::unordered_map<std::string, Entry> r;
    return r;
}
Entry g_table[HLE_D3D__COUNT];
std::once_flag g_table_once;

void BuildTable() {
    int missing = 0;
    for (int i = 0; i < HLE_D3D__COUNT; i++) {
        auto it = Registry().find(g_hle_d3d_info[i].name);
        if (it == Registry().end()) {
            missing++;
            continue;
        }
        g_table[i] = it->second;
        if (it->second.kind == hle::kReplace && it->second.stack_args != g_hle_d3d_info[i].stack_args)
            std::fprintf(stderr, "[HLE] W %s: %d stack args here, %d in the function list\n",
                         g_hle_d3d_info[i].name, it->second.stack_args, g_hle_d3d_info[i].stack_args);
    }
    std::fprintf(stderr, "[HLE] I D3D entry points: %d of %d implemented on the host\n",
                 HLE_D3D__COUNT - missing, HLE_D3D__COUNT);
}

int Run(const Entry& e) {
    X86Context& ctx = kernel::CurrentContext();
    ::kernel::Args args{ctx};
    uint32_t r = e.fn(ctx, args);
    if (e.kind == hle::kBefore) return 0;
    ::kernel::Return(ctx, r, e.stack_args);
    return 1;
}
}  // namespace

hle::Registration::Registration(const char* name, Fn fn, int stack_args, Kind kind) {
    Registry()[name] = Entry{fn, stack_args, kind};
}

extern "C" int hle_d3d_dispatch(int id) {
    std::call_once(g_table_once, BuildTable);
    const Entry& e = g_table[id];
    return e.fn ? Run(e) : 0;
}

// a function without a generated wrapper (D3DDevice_Swap: recomp_manual.c)
extern "C" int hle_d3d_dispatch_named(const char* name) {
    auto it = Registry().find(name);
    return it != Registry().end() ? Run(it->second) : 0;
}

// ---- frame pacing ----------------------------------------------------------

// the XDK's presentation interval (D3D global 0x4AB5CC, set per swap path;
// recomp_manual.c lowers 2 to 1 for the 60 FPS option): wait for the next
// slot of interval / 60 s after the previous present
void app::Pace() {
    using clock = std::chrono::steady_clock;
    static clock::time_point next;
    uint32_t interval = kernel::R32(0x004AB5CCu) & 0x7FFFFFFFu;
    if (interval < 1 || interval > 4) interval = 1;
    auto period = std::chrono::microseconds(16667 * interval);
    auto now = clock::now();
    if (next.time_since_epoch().count() == 0 || now - next > period) next = now;
    next += period;
    if (next > now) std::this_thread::sleep_until(next);
}

// ---- null backend (M2) ------------------------------------------------------

namespace {
class NullBackend : public gpu::Backend {
public:
    bool Init(uint32_t w, uint32_t h) override {
        std::fprintf(stderr, "[HLE] I null backend %ux%u: D3D handled, nothing drawn\n", w, h);
        return true;
    }
    void Clear(const gpu::State&, uint32_t, const uint8_t*, uint32_t, uint32_t, float, uint32_t) override { clears_++; }
    void Draw(const gpu::State&, uint32_t, uint32_t, uint32_t count, const uint8_t*) override {
        draws_++;
        verts_ += count;
    }
    void Copy(const gpu::Surface&, const gpu::Surface&, uint32_t, uint32_t, uint32_t) override { copies_++; }
    void Present(const gpu::Surface&, bool) override {
        presents_++;
        using clock = std::chrono::steady_clock;
        auto now = clock::now();
        if (last_.time_since_epoch().count() == 0) last_ = now;
        if (now - last_ >= std::chrono::seconds(5)) {
            std::fprintf(stderr, "[HLE-FRAME] 5 s: %u presents, %u draws (%llu vertices), %u clears, %u copies\n",
                         presents_, draws_, (unsigned long long)verts_, clears_, copies_);
            presents_ = draws_ = clears_ = copies_ = 0;
            verts_ = 0;
            last_ = now;
        }
    }
    void Sync(uint32_t, uint32_t) override {}
    void SyncLate(uint32_t) override {}
    void Flush() override {}
    void SetGamma(const uint8_t*) override {}

private:
    uint32_t presents_ = 0, draws_ = 0, clears_ = 0, copies_ = 0;
    uint64_t verts_ = 0;
    std::chrono::steady_clock::time_point last_;
};
}  // namespace

std::unique_ptr<gpu::Backend> gpu::CreateBackend() { return std::make_unique<NullBackend>(); }
std::unique_ptr<gpu::Backend> gpu::CreateThreadedBackend(std::unique_ptr<gpu::Backend> inner) { return inner; }
void gpu::SavePipelineCache() {}
