# MCPX APU Emulation Library
# Extracted from xemu (https://github.com/xemu-project/xemu)
#
# Voice Processor (VP): ADPCM/PCM decode, sample mixing, envelopes, filters
# Global Processor (GP): Stub - effects bypass
# Encode Processor (EP): Stub - direct mixbin passthrough

add_library(xbox_apu STATIC
    apu_core.c
    apu_vp.c
    apu_dsp.c
    apu_mmio_hook.c
    apu_xaudio2.c
    apu_surround.c
    apu_passthrough.c
)

target_include_directories(xbox_apu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# qemu_shim.h lives in nv2a/ - shared QEMU type abstraction
target_include_directories(xbox_apu PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../nv2a
)

if(MSVC)
    target_compile_options(xbox_apu PRIVATE
        /W3
        /wd4244  # conversion warnings (float/double/int)
        /wd4146  # unary minus on unsigned
        /wd4267  # size_t to int conversion
    )
    target_compile_definitions(xbox_apu PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _USE_MATH_DEFINES
    )
endif()

# DSP56300 core for the GP/EP (xemu hw/xbox/mcpx/apu/dsp at f9b14039, C
# interpreter backend only; see dsp/PROVENANCE.md -- GPL v2+ / LGPL v2+).
# Built as its own library so it compiles against the small local shims in
# dsp/shim/ instead of QEMU, and nothing else sees those shims.
add_library(xbox_apu_dsp STATIC
    dsp/dsp.c
    dsp/dsp_c.c
    dsp/dsp_dma.c
    dsp/dsp_jit_stub.c
    dsp/dsp_diag.c
    dsp/interp/dsp_cpu.c
    dsp/interp/debug.c
)
target_include_directories(xbox_apu_dsp
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/dsp
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/dsp/shim
            ${CMAKE_CURRENT_SOURCE_DIR}/dsp/interp
)
if(MSVC)
    target_compile_options(xbox_apu_dsp PRIVATE /W3 /wd4244 /wd4267 /wd4146)
    target_compile_definitions(xbox_apu_dsp PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# Regression tests for the DSP port (not part of the default build).
add_executable(apu_dsp_tests EXCLUDE_FROM_ALL
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/apu_dsp/test_apu_dsp.c)
target_include_directories(apu_dsp_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../nv2a
    ${CMAKE_CURRENT_SOURCE_DIR}/dsp/shim)
target_link_libraries(apu_dsp_tests PRIVATE xbox_apu platform)
if(MSVC)
    target_compile_definitions(apu_dsp_tests PRIVATE _CRT_SECURE_NO_WARNINGS
                               _USE_MATH_DEFINES)
endif()

# libsamplerate 0.2.2 (vendored, BSD-2, see third_party/libsamplerate/
# PROVENANCE.md): xemu's VP resamples each voice from its pitch rate to
# 48 kHz with SRC_SINC_FASTEST (docs Update 429). Only the fast sinc
# converter is enabled; x86 has no clip-mode behaviour.
add_library(xbox_samplerate STATIC
    third_party/libsamplerate/samplerate.c
    third_party/libsamplerate/src_sinc.c
    third_party/libsamplerate/src_linear.c
    third_party/libsamplerate/src_zoh.c
)
target_include_directories(xbox_samplerate PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libsamplerate)
target_compile_definitions(xbox_samplerate PRIVATE
    ENABLE_SINC_FAST_CONVERTER=1 CPU_CLIPS_POSITIVE=0 CPU_CLIPS_NEGATIVE=0
    HAVE_STDBOOL_H=1 PACKAGE="libsamplerate" VERSION="0.2.2")
if(MSVC)
    target_compile_options(xbox_samplerate PRIVATE /W3 /wd4244 /wd4267 /wd4305)
    target_compile_definitions(xbox_samplerate PRIVATE _CRT_SECURE_NO_WARNINGS
                               _USE_MATH_DEFINES)
endif()
target_link_libraries(xbox_apu PUBLIC xbox_samplerate)

# Opt-in surround path (RECOMP_AUDIO_SURROUND=1, docs Update 611): the GP/EP
# on mborgerson/dsp56300 (MIT, loaded from dsp56300.dll, see
# dsp_jit/build_dsp56300_dll.ps1) with xemu's surround-branch dsp.c/dma.c
# (GPL v2+ / LGPL v2+, renamed dspjit_* by the forced include so they can sit
# next to the interpreter copies above), and liba52 (GPL v2+) to decode the
# EP's AC-3 output. See dsp_jit/PROVENANCE.md, third_party/a52dec/PROVENANCE.md.
add_library(xbox_apu_dspjit STATIC
    dsp_jit/dsp.c
    dsp_jit/dma.c
    dsp_jit/dsp56300_dll.c
)
target_include_directories(xbox_apu_dspjit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/dsp_jit
    ${CMAKE_CURRENT_SOURCE_DIR}/dsp_jit/shim)
if(MSVC)
    target_compile_options(xbox_apu_dspjit PRIVATE /W3 /wd4244 /wd4267 /wd4146
        "/FI${CMAKE_CURRENT_SOURCE_DIR}/dsp_jit/dspjit_rename.h")
    target_compile_definitions(xbox_apu_dspjit PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_library(xbox_a52 STATIC
    third_party/a52dec/liba52/bitstream.c
    third_party/a52dec/liba52/imdct.c
    third_party/a52dec/liba52/bit_allocate.c
    third_party/a52dec/liba52/parse.c
    third_party/a52dec/liba52/downmix.c
)
target_include_directories(xbox_a52
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/a52dec/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/a52dec)
if(MSVC)
    target_compile_options(xbox_a52 PRIVATE /W3 /wd4244 /wd4305 /wd4267 /wd4018)
    target_compile_definitions(xbox_a52 PRIVATE _CRT_SECURE_NO_WARNINGS
                               _USE_MATH_DEFINES)
endif()
target_link_libraries(xbox_apu PUBLIC xbox_apu_dspjit xbox_a52)

# apu_dsp.c (the adapted xemu gp_ep.c) reads g_config via the shim header.
target_include_directories(xbox_apu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/dsp/shim)
target_link_libraries(xbox_apu PUBLIC xbox_apu_dsp)

if(WIN32)
    target_link_libraries(xbox_apu PRIVATE platform kernel32 winmm)
else()
    # Linux: platform layer (Win32 primitives on POSIX) + SDL2 audio + libm
    find_package(SDL2 REQUIRED)
    target_link_libraries(xbox_apu PRIVATE platform SDL2::SDL2 m)
endif()
