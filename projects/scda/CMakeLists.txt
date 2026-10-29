cmake_minimum_required(VERSION 3.20)
project(scda_recomp C)

# Make F5 launch the actual game host instead of CMake's non-executable
# ALL_BUILD utility target when the generated solution is opened in VS.
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT scda_recomp)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_LIST_DIR}/../..")
# Seed 33 adds 0x00046903, the shared continuation/epilogue that
# sub_00046887 reaches via a direct tail jump at native 0x000468BC. It fell
# inside the disassembled range of the unrelated adjacent function
# sub_000468DF and was emitted as an unresolved stub, so the pushed
# ebx/esi/edi never got restored; the caller (sub_00047797, invoked from
# sub_00072CBD's virtual dispatch) popped a stale nearby value
# (0x004F7248, a file-manager VTable address) into ebx instead of 0,
# defeating sub_00072CBD's `esi == ebx` null check and producing the
# persistent source=0x00000009 vector-copy loop.
#
# Seed 34 adds 0x00368DC2, a second, independent unresolved-stub tail-jump
# target (reached via sub_0004721E -> sub_000678C0 -> 0x00368DC2) in a 23-byte
# gap between sub_00368BFD and sub_00368DD9 that the disassembler never saw
# as a function start. The stub did nothing and left the guest stack pointer
# 16 bytes shallower than the real function (push 1; re-push 3 args; call
# sub_00368BFD; add esp,0x10; ret) would have, which shifted sub_0004721E's
# own epilogue pops by one slot and produced the exact same defeated
# `esi == ebx` null check in sub_00072CBD as Seed 33, by a different route.
# See projects/scda/function_seeds.json and docs/SCDA_WORKLOG.md.
#
# Seed 35 adds 0x00075667, a virtual-method destructor reached only through
# a vtable data pointer (not a jump/call xref), sitting in a 260-byte gap
# between sub_00075577 and sub_000756E5 that no disassembler heuristic ever
# looked at. RECOMP_ICALL_SAFE's failure path silently reset the guest
# stack pointer instead of crashing, masking the failure but leaving
# sub_00076402 (in the actor/child-list linking code reached from
# sub_00246CB4) one dword shallower than it should be -- the same
# stack-shallow signature as Seeds 33/34, this time reached purely through
# vtable data. See projects/scda/function_seeds.json and docs/SCDA_WORKLOG.md.
#
# Seed 36 adds 0x0007291B, a 5-byte unresolved tail-jump thunk (`jmp
# sub_00051D2D`) reached only through a vtable data pointer stored by
# sub_0007292A -- the same "plain jmp thunk" shape as Seeds 33/34 combined
# with the same "vtable data_imm only" shape as Seed 35. Its icall failure
# masked via RECOMP_ICALL_SAFE inside sub_00045C06, leaving an uninitialized
# value to flow into the next call (sub_000371A6, a generic wide-string
# compare helper) as its string pointer, which then walked off into
# unmapped memory and crashed. See projects/scda/function_seeds.json and
# docs/SCDA_WORKLOG.md.
#
# 2026-09-01: regenerated against the merged v0.6.0 upstream disasm/recomp
# tools (branch upgrade/v0.6.0-merge). The improved function-boundary
# heuristic (functions.py's max_target fix) alone found 716 more functions
# automatically (28,419 -> 29,135 detected) without any new seeds, and
# unresolved stubs dropped from 1,909 to 1,297. Seeds 33-36 above remain in
# function_seeds.json and are still applied, but may now be partially or
# fully redundant with the improved detector -- not yet re-verified either
# way. See docs/SCDA_WORKLOG.md for the merge's full rationale.
set(SCDA_GEN_DIR "${XBOXRECOMP_DIR}/tools/recomp/output/scda_v060/gen")

if(NOT EXISTS "${SCDA_GEN_DIR}/recomp_funcs.h")
    message(FATAL_ERROR "Generate SCDA code first; see docs/SCDA_WORKLOG.md")
endif()

# 2026-09-03 (continued 26): v0.7.x-merge regression A/B trace comparison.
# When ON, swaps the trace handler to scda_ab_trace_pc (projects/scda/src/
# scda_ab_trace.c) -- a minimal, fast per-basic-block logger with none of
# scda_trace_pc()'s diagnostic branching -- and widens the compile-time
# trace range to bracket early boot. Build this exact flag identically for
# the OLD (checkpoint/scda-pre-v0.7-merge) and NEW builds being compared;
# the two resulting scda_ab_trace.log files are meant for a local, offline
# diff script, not for loading into a model context. See
# docs/SCDA_WORKLOG.md, "continued 26".
option(SCDA_AB_TRACE "Emit a compact per-basic-block A/B trace log instead of the normal diagnostic hooks" OFF)

if(NOT WIN32)
    # ARM port (docs Update 677): 16-bit wchar_t like the runtime's WCHAR
    add_compile_options($<$<COMPILE_LANGUAGE:C>:-fshort-wchar>)
endif()
add_subdirectory("${XBOXRECOMP_DIR}" "${CMAKE_BINARY_DIR}/xboxrecomp")
file(GLOB SCDA_GENERATED CONFIGURE_DEPENDS "${SCDA_GEN_DIR}/*.c")

if(NOT SCDA_AB_TRACE)
    # RECOMP_ABI_CHECK on scda_recomp alone isn't enough: recomp_abi_
    # violation_log's own definition (src/kernel/xbox_memory_layout.c) is
    # itself gated behind #ifdef RECOMP_ABI_CHECK, and xbox_kernel is a
    # separate target that doesn't inherit scda_recomp's compile
    # definitions. Without this, every generated .c that calls
    # RECOMP_ABI_CALL links against a symbol the kernel library never
    # actually compiled in.
    target_compile_definitions(xbox_kernel PRIVATE RECOMP_ABI_CHECK)
endif()

# the HLE renderer is C++ (agent_handoff/HLE_RENDERER_PLAN.md)
enable_language(CXX)
add_executable(scda_recomp
    src/main.c
    src/recomp_manual.c
    src/scda_input.c
    src/scda_options.c
    src/scda_layout.c
    # HLE renderer entry points and core (agent_handoff/HLE_RENDERER_PLAN.md)
    src/hle_d3d_wrappers.c
    src/hle_d3d.c
    ${SCDA_GENERATED}
)
if(SCDA_AB_TRACE)
    target_sources(scda_recomp PRIVATE src/scda_ab_trace.c)
endif()

# HLE renderer: host implementations of the title's Direct3D 8 calls, as an
# object library of its own (objects always linked: d3d8.cpp registers its
# functions from static constructors) whose include paths stay away from the C
# sources.
# hle/tgp holds sources from SCDA-XBERecomp (MIT, (c) 2026 TGP482,
# Joshhhuaaa, see hle/tgp/LICENSE and PROVENANCE.md); hle/compat maps their
# runtime calls onto this one (BEFORE: compat's kernel/kernel.h etc. must win
# over the runtime's headers of the same name).
add_library(scda_hle OBJECT
    src/hle/hle_glue.cpp
    src/hle/tgp/xdk/d3d8.cpp
    src/hle/tgp/gpu/texture.cpp
    src/hle/tgp/gpu/vertex.cpp
    src/hle/tgp/gpu/shaders.cpp
)
target_include_directories(scda_hle BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/hle/compat
    ${CMAKE_CURRENT_SOURCE_DIR}/src/hle/tgp
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(NOT WIN32)
    target_include_directories(scda_hle BEFORE PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/hle/compat_posix)
endif()
set_target_properties(scda_hle PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
if(MSVC)
    target_compile_options(scda_hle PRIVATE /W3 /utf-8)
    target_compile_definitions(scda_hle PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()
target_link_libraries(scda_recomp PRIVATE scda_hle)

# templates/runtime first, so hand-written sources get the template. The
# generated .c files include "recomp_types.h" with quotes and therefore always
# see the copy in the gen directory first -- that copy must match the template
# (the generator writes it when absent and warns when it differs; a stale copy
# from 2026-09-23 hid recomp_rdtsc and a TLS declaration, docs Update 346).
target_include_directories(scda_recomp PRIVATE
    "${XBOXRECOMP_DIR}/templates/runtime"
    "${SCDA_GEN_DIR}"
    "${XBOXRECOMP_DIR}/include"
)
target_link_libraries(scda_recomp PRIVATE xboxrecomp)

# 2026-09-03 (continued 20): trimmed down from the near-full-.text watchpoint
# used throughout the EDI/sub_0004612B bisection (see docs/SCDA_WORKLOG.md,
# "continued 13-19") now that the root cause is proven and fixed -- that
# investigation is CLOSED, do not re-widen these ranges to re-chase it.
# Only two narrow windows remain, matching the lean final-verification hook
# set in scda_trace_pc(): sub_001D5208's own entry/type-lookup checkpoint,
# and the 0x4909F/0x490BC registry bounds-check.
if(SCDA_AB_TRACE)
    # Wide enough to bracket xbe_entry_point through well past the old
    # sub_001D5208 checkpoint; narrower than a literal full-.text range
    # (which is what made earlier sessions' broad traces take 8+ minutes).
    target_compile_definitions(scda_recomp PRIVATE
        SCDA_ICALL_PROBE
        RECOMP_TRACE_HANDLER=scda_ab_trace_pc
        RECOMP_TRACE_START=0x00000000u
        RECOMP_TRACE_END=0x00500000u
    )
else()
    target_compile_definitions(scda_recomp PRIVATE
        SCDA_ICALL_PROBE
        SCDA_GPU_EMIT_TRACE
        # 2026-09-03 (continued 26): v0.7.x regression hunt, step 1 -- names
        # the first function that clobbers ebx/esi/edi across an indirect (or
        # direct) call. Covers direct calls too via RECOMP_ABI_CALL, since
        # recomp emits every direct call through that macro now. See
        # docs/SCDA_WORKLOG.md.
        RECOMP_ABI_CHECK
        RECOMP_TRACE_HANDLER=scda_trace_pc
        RECOMP_TRACE_START=0x0004909Fu
        RECOMP_TRACE_END=0x000490C0u
        RECOMP_TRACE_START_7=0x001D5208u
        RECOMP_TRACE_END_7=0x001D5620u
        # 2026-09-03 (continued 22): sub_000556A8's LaunchDataPage payload copy
        # -- new lead following the closed EDI bug (self-relaunch via
        # HalReturnToFirmware(2)). See docs/SCDA_WORKLOG.md.
        # 2026-09-03 (continued 24): reused this slot (was 0x5573C
        # LaunchDataPage payload dump, already confirmed never firing) for the
        # sub_00055187 full-function bisection -- see [SCDA-55187-BLOCK].
        # RETRACTED per "continued 24" -- the sub_00055187/EDI hypothesis
        # chain is closed.
        # 2026-09-04 (continued 29): reused again for the post-0x0048A767-fix
        # blocker (sub_000412FE's virtual dispatch through [eax+0x48] reading
        # a garbage vtable pointer) -- CLOSED. Root cause turned out to be
        # static (a codegen bug in tools/recomp/translator.py's
        # _fixup_icall_esp_save, fixed generically there), found from the
        # generated C alone before this hook ever needed to fire.
        # 2026-09-04 (continued 34-35): was 0x004A0F90-0x004A1300, tracing
        # the NV2A base-pointer setup chain. CLOSED: confirmed (via a clean
        # ~*k dump of all 5 threads, correcting an earlier frame-walk
        # misread) that the sub_004A12B0 spin runs on the MAIN thread, not
        # a worker thread -- the real cause is that the global D3D device
        # struct at 0x4AB668 is still NULL when the main thread's D3D
        # push-buffer flush code (sub_0049CE00 -> ... -> sub_004A1740 ->
        # sub_004A1600 -> sub_004A12B0) runs, i.e. device creation
        # (sub_0049D210) has not happened yet in program order. Not a
        # memory/pointer semantics or NV2A-hardware-emulation bug (the
        # existing NV2A_ACK mechanism already handles this exact
        # register/bit correctly). See docs/SCDA_WORKLOG.md, continued 35.
        # 2026-09-04 (continued 36): does sub_0049D210 (device creation)
        # ever run before sub_0049CE00 is reached with a null device? See
        # [SCDA-DEVCREATE] and docs/SCDA_WORKLOG.md.
        # 2026-09-04 (continued 36): was used for several rounds checking
        # the D3D device-creation-order hypothesis (0x0049CE00-0x0049D220,
        # then 0x0003FCA5, then 0x002E75CE). Found and fixed one real bug
        # along the way (sub_002E7381, a constructor lost in a disasm gap)
        # but it did not resolve the sub_004A12B0 spin -- struct_base is
        # still 0 there. Root cause of *that* remains open: sub_0049CE00
        # itself has no null-check on the device pointer, and it is not
        # yet established whether reaching it with a null device is itself
        # abnormal (something upstream should have gated it) or normal-but-
        # differently-answered (real hardware's low guest memory holds
        # real kernel data at the offsets this function reads through a
        # null base, ours reads zeroed RAM, so the same "null device" state
        # takes a different branch there). See docs/SCDA_WORKLOG.md,
        # continued 36. Idle again.
        # 2026-09-04 (continued 38): xemu-confirmed real caller chain
        # sub_002E75CE -> sub_002E2BB1 -> sub_0049D210 (device creation).
        # Checking whether sub_002E75CE/sub_002E2BB1 now fire in
        # scda_recomp after the sub_002E7381 constructor fix. See
        # [SCDA-VTABLE-CHAIN] and docs/SCDA_WORKLOG.md.
        # 2026-09-04 (continued 38): xemu-confirmed the real caller of
        # sub_002E75CE is the call site at 0x00049816 (`call [eax+0x68]`)
        # inside sub_0004934E, a 5202-byte vtable-only dispatcher (itself
        # detection_method seed_vtable_thunk -- part of the 1,419-entry
        # CRT constructor/atexit table from continued 32/33). Checking
        # whether sub_0004934E/this call site ever run in scda_recomp.
        # 2026-09-04 (continued 38): was 0x00048B15-0x0004A7A0. Confirmed
        # via xemu (QEMU gdbstub, tools/debug/xemu_4ab668_watchpoint.py and
        # xemu_2e75ce_caller.py) that the real device-creation trigger on
        # hardware is sub_00048B15 -> ... -> sub_0004934E (a vtable-only
        # dispatcher, call site 0x00049816) -> sub_002E75CE -> sub_002E2BB1
        # -> sub_0049D210. In scda_recomp, sub_00048B15 reaches 0x48BBE
        # (status140 <- 1, confirmed live) but NEVER reaches 0x48EC9/0x48ED1
        # just ~0x300 bytes later -- narrowed but not pinned to one of the
        # calls in between (sub_00042082, sub_00067886, sub_0003E9D9,
        # sub_00045647, sub_00045584, sub_00043455, two vtable calls,
        # sub_00044C15, two more vtable calls, sub_001CAD08, sub_001CB603,
        # sub_001D589D, sub_00070B06, sub_000371A6, sub_000370F1,
        # sub_00067886, sub_001D47CF, sub_00070854). No `ret` exists in that
        # span, so control leaves without an ordinary return -- that hunch was
        # WRONG, see continued 39: the span has no `ret` because it legitimately
        # calls into the pre-existing sub_004A12B0 spin and never returns from
        # that, not because of a translation bug.
        # 2026-09-05 (continued 41): bisected sub_00045D0B/sub_00045E42's
        # branches against real hardware -- found the sub_004A12B0 "spin" is
        # very likely a huge-but-finite loop (169648 iterations vs real
        # hardware's 363-8610) driven by a LEB128-style count decoded from a
        # stream by sub_0005FBDA. Root cause (why the decoded count is wrong)
        # not yet found -- see docs/SCDA_WORKLOG.md.
        # 2026-09-05 (continued 44): dumped the UMOD-reader object's fields
        # and compared against real hardware -- confirmed the reader's read
        # position is wrong (0x14 vs the correct 0x712FF0/info_off) at the
        # moment sub_00045D0B's call needs it. See docs/SCDA_WORKLOG.md.
        # 2026-09-05 (continued 49): the sub_004A12B0 bisection hook this
        # window was added for got reverted before it was ever exercised
        # under real conditions -- the file-lookup fix that would have let
        # boot reach that code turned out to feed wrong-content data (see
        # docs/SCDA_WORKLOG.md). Idle again; the exact address range
        # (0x4A12B0-0x4A1800) is preserved in that entry for whoever picks
        # this back up once the underlying game-data question is settled.
        # 2026-09-05 (continued 50): traced the "menu" path segment all the
        # way to sub_00048B15's map-array lookup (correctly populated,
        # count=32) and confirmed the common.lin open's failure is treated
        # as non-fatal immediately afterward -- this whole thread turned out
        # to be a red herring, not the disc-error trap's actual cause. See
        # docs/SCDA_WORKLOG.md. Idle again.
        # 2026-09-05 (continued 51): traced the disc-error trap to its real
        # root cause (xbox_NtCreateFile's missing STATUS_FILE_IS_A_DIRECTORY
        # check, fixed in kernel_file.c) -- idle again. See
        # docs/SCDA_WORKLOG.md.
        # 2026-09-06 (continued 52): tried an address-independent watcher on
        # MEM32(0x4AB668) over a near-full-.text window (0x0-0x500000) to
        # find what corrupts it on the second sub_004A12B0 call path --
        # ZERO hits over the entire run, even reaching the spin, meaning the
        # write does not happen through any traced guest instruction at all.
        # Points to a host-side (kernel bridge / loader) write, or to the
        # value being a legitimate large pointer whose apparent "garbage"
        # look is this project's own documented virtual-vs-physical
        # aliasing gap (kernel_bridge.c's NtAllocateVirtualMemory comment),
        # not a new bug. See docs/SCDA_WORKLOG.md. Idle again.
        # 2026-09-06 (continued 57): [SCDA-77487-POOLCHECK] -- checked the
        # free-list allocator capacity test inside sub_00077194 that gates
        # the sub_0049CE00 call (a CRT heap-coalescing edge case, not a
        # device-creation issue at all -- see docs/SCDA_WORKLOG.md). Idle
        # again.
        # 2026-09-06 (continued 58): [SCDA-774E6-MALLOC] -- traced the
        # oversized malloc() requests to their exact size and caller,
        # confirming the link to the 750MB Z:-partition heap-alloc failure.
        # See docs/SCDA_WORKLOG.md. Idle again.
        # 2026-09-06 (continued 59): traced the 750MB request all the way to
        # xbox_NtQueryInformationFile's GetFileSize() answer on the raw
        # Partition5 device handle -- see docs/SCDA_WORKLOG.md. Idle again.
        # 2026-09-06 (continued 63): counted sub_0004721E's calls (745,
        # matching real hardware's own frequent use) and pinned the one
        # failing call to obj2=MEM32(0x5B57FC) holding a stack address --
        # connects to a pre-existing 2026-09-02/03 stack-corruption
        # investigation in this exact call chain. See
        # docs/SCDA_WORKLOG.md. Idle again.
        # 2026-09-09: continued 63's own next step, re-run on the CURRENT
        # boot path (which no longer reaches sub_00071B49/sub_0004023A at
        # all -- confirmed via live cdb breakpoints, zero hits). Found: a
        # freshly-constructed file-manager object (vtable 0x004F7248, built
        # by sub_00047505) is enumerated by sub_0004721E with
        # container_base=0 container_buckets=0 container_mask=0 -- an
        # entirely empty container, not a corrupted one. See
        # docs/SCDA_WORKLOG.md. Idle again.
        # 2026-09-10 (new session): tried activating the existing
        # [SCDA-ABC]/[SCDA-VCALL] hooks (0x47440/0x4744A/0x47456 in
        # sub_0004721E's primary get-name lookup, 0x48C89/0x48CAA/0x48CCB/
        # 0x48CD0 in sub_00048B15's MEM32(0x5B5800) write) -- confirmed
        # dormant in the default (non-SCDA_AB_TRACE) build because none of
        # the active windows covered this address range. Reverted: even
        # this narrow bracket forced a full rebuild (RECOMP_TRACE_START_2
        # is a preprocessor define touching every generated recomp_*.c
        # translation unit) that ran over an hour without finishing --
        # abandoned in favor of a pure cdb register-based approach (no
        # rebuild needed) on the existing canonical binary. See
        # docs/SCDA_WORKLOG.md.
        # 2026-09-26 (docs Update 346): sub_0029B36A, which registers one UI
        # screen (.mgb) per call from sub_00290A8C and registers none in
        # recomp (xemu: 11). Earlier this window covered sub_0017BF36 ..
        # sub_00185C2F (proven: recomp follows xemu's chain into them).
        # 2026-09-28 (docs Update 356): in-level save never requests the slot
        # write: blocks of sub_0018A40D (save prep, calls sub_002944F7 at
        # 0x18A6E8 behind two checks). Range 4 below: sub_002944F7 entry.
        RECOMP_TRACE_START_2=0x0018A40Du
        RECOMP_TRACE_END_2=0x0018A6F7u
        # 2026-09-03 (continued 23): sub_00055666 entry, the reboot-gate check
        # -- confirmed the retail XBE and xemu both have 0x5B1D94=0x0047D997
        # baked in, and our loader reproduces that correctly right after XBE
        # load; this hook checks whether it's still correct by the time the
        # game itself reads it. See docs/SCDA_WORKLOG.md.
        RECOMP_TRACE_START_6=0x00055666u
        RECOMP_TRACE_END_6=0x00055667u
        RECOMP_TRACE_START_5=0x00054F57u
        RECOMP_TRACE_END_5=0x00054F58u
        RECOMP_TRACE_START_4=0x002944F7u
        RECOMP_TRACE_END_4=0x002944F8u
        RECOMP_TRACE_START_3=0x0047D997u
        RECOMP_TRACE_END_3=0x0047D998u
    )
endif()

if(MSVC)
    # The generated SCDA translation units are deliberately compiled in
    # parallel.  /FS serializes their shared compiler-PDB writes and avoids
    # MSVC C1041 races in RelWithDebInfo builds.
    target_compile_options(scda_recomp PRIVATE /bigobj /W3 /FS)
    # docs Update 642: /Ob2 /Oi /GS- for the generated code was measured at
    # the JBA save without any FPS gain (45.5 vs 45.9).
    # docs Update 644: MSVC silently skips optimisation of very large
    # functions (C4883, hidden by /w) -- hot render functions such as
    # sub_0021B0DD were compiled like /Od. /d2OptimizeHugeFunctions lifts
    # that limit.
    set_source_files_properties(${SCDA_GENERATED} PROPERTIES
        COMPILE_OPTIONS "/w;/d2OptimizeHugeFunctions")
    target_link_libraries(scda_recomp PRIVATE dbghelp)
    target_link_options(scda_recomp PRIVATE /LARGEADDRESSAWARE /MAP
        $<$<CONFIG:Debug>:/DEBUG:FULL>)
    set_property(TARGET scda_recomp PROPERTY
        MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug>:ProgramDatabase>")
    set_target_properties(scda_recomp PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "${XBOXRECOMP_DIR}"
        VS_DEBUGGER_ENVIRONMENT "XBOX_LOG_LEVEL=4")
endif()

if(NOT MSVC)
    # Linux (ARM port, docs Update 677). The generated code at -O1 without
    # debug info (2.7 M lines; -O2 -g needs hours and more RAM than the
    # board has), with MSVC's aliasing and overflow semantics.
    set_source_files_properties(${SCDA_GENERATED} PROPERTIES
        COMPILE_OPTIONS "-w;-O1;-g0;-fno-strict-aliasing;-fwrapv")
    target_compile_options(scda_recomp PRIVATE -fno-strict-aliasing -fwrapv)
    target_link_libraries(scda_recomp PRIVATE m dl pthread)
endif()

set_target_properties(scda_recomp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Runtime DLLs of the Vulkan renderer (glslang, src/gpu/CMakeLists.txt).
if(TARGET xbox_gpu)
    get_target_property(_gpu_dlls xbox_gpu XBOX_RUNTIME_DLLS)
    if(_gpu_dlls)
        add_custom_command(TARGET scda_recomp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_gpu_dlls}
                    $<TARGET_FILE_DIR:scda_recomp>
            COMMENT "Copying Vulkan renderer runtime DLLs")
    endif()
endif()
