# NV2A GPU emulation: xemu's PFIFO/PGRAPH core and OpenGL renderer
# (xemu 0.8.66, see PROVENANCE.md), built against the local shims in shim/
# instead of QEMU, plus the glue that attaches it to the recompiled title
# (nv2a_recomp.c). Enabled at runtime with RECOMP_NV2A_XEMU=1.

set(XEMU ${CMAKE_CURRENT_SOURCE_DIR}/xemu)
set(NV2A ${XEMU}/hw/xbox/nv2a)

add_library(xbox_gpu STATIC
    # NV2A register blocks and command FIFO
    ${NV2A}/pfifo.c
    ${NV2A}/pmc.c
    ${NV2A}/pbus.c
    ${NV2A}/pcrtc.c
    ${NV2A}/pfb.c
    ${NV2A}/pramdac.c
    ${NV2A}/ptimer.c
    ${NV2A}/pvideo.c
    ${NV2A}/stubs.c
    ${NV2A}/user.c
    ${NV2A}/prmcio.c
    ${NV2A}/prmdio.c
    ${NV2A}/prmvio.c
    # PGRAPH (renderer-independent)
    ${NV2A}/pgraph/pgraph.c
    ${NV2A}/pgraph/texture.c
    ${NV2A}/pgraph/swizzle.c
    ${NV2A}/pgraph/s3tc.c
    ${NV2A}/pgraph/shaders.c
    ${NV2A}/pgraph/vertex.c
    ${NV2A}/pgraph/profile.c
    ${NV2A}/pgraph/rdi.c
    # GLSL generators (shared with a later Vulkan backend)
    ${NV2A}/pgraph/glsl/common.c
    ${NV2A}/pgraph/glsl/geom.c
    ${NV2A}/pgraph/glsl/psh.c
    ${NV2A}/pgraph/glsl/vsh-ff.c
    ${NV2A}/pgraph/glsl/vsh-prog.c
    ${NV2A}/pgraph/glsl/vsh.c
    # OpenGL renderer
    ${NV2A}/pgraph/gl/blit.c
    ${NV2A}/pgraph/gl/debug.c
    ${NV2A}/pgraph/gl/display.c
    ${NV2A}/pgraph/gl/draw.c
    ${NV2A}/pgraph/gl/renderer.c
    ${NV2A}/pgraph/gl/reports.c
    ${NV2A}/pgraph/gl/shaders.c
    ${NV2A}/pgraph/gl/surface.c
    ${NV2A}/pgraph/gl/texture.c
    ${NV2A}/pgraph/gl/vertex.c
    ${NV2A}/pgraph/thirdparty/gloffscreen/common.c
    # QEMU utilities and the CPU vertex-shader emulator xemu links
    ${XEMU}/util/fast-hash.c
    ${XEMU}/util/mstring.c
    ${XEMU}/subprojects/nv2a_vsh_cpu/nv2a_vsh_cpu.c
    ${XEMU}/subprojects/nv2a_vsh_cpu/nv2a_vsh_disassembler.c
    ${XEMU}/subprojects/nv2a_vsh_cpu/nv2a_vsh_emulator.c
    ${XEMU}/subprojects/nv2a_vsh_cpu/nv2a_vsh_emulator_execution_state.c
    # Local glue
    gloffscreen_wgl.c
    gl_loader.c
    nv2a_recomp.c
)

target_include_directories(xbox_gpu PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/shim
    ${CMAKE_CURRENT_SOURCE_DIR}/shim/epoxy
    ${XEMU}/include
    ${XEMU}
    ${NV2A}
    ${NV2A}/pgraph/thirdparty/gloffscreen
    ${XEMU}/subprojects/nv2a_vsh_cpu
)
target_include_directories(xbox_gpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    # /std:clatest: typeof (gl/shaders.c) is only accepted in C23 mode.
    target_compile_options(xbox_gpu PRIVATE
        /std:clatest /W3
        /wd4244 /wd4267 /wd4146 /wd4018 /wd4305 /wd4101 /wd4996)
    target_compile_definitions(xbox_gpu PRIVATE
        _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)
endif()

# xxHash (shim/xxhash.h, BSD-2) is used header-only.
set_source_files_properties(${XEMU}/util/fast-hash.c PROPERTIES
    COMPILE_DEFINITIONS XXH_INLINE_ALL)

# Synchronization: WaitOnAddress for the PFIFO kick (pfifo.c, docs Update 643).
target_link_libraries(xbox_gpu PRIVATE opengl32 gdi32 user32 Synchronization)

# ── Vulkan renderer (xemu pgraph/vk), selected at runtime with
# RECOMP_RENDERER=vulkan; OpenGL stays the default. Everything it needs is
# vendored under thirdparty/ (see PROVENANCE.md): Khronos Vulkan headers,
# volk (loads vulkan-1.dll at runtime, so no Vulkan SDK is needed to build),
# VulkanMemoryAllocator, SPIRV-Reflect, and glslang's C interface. glslang
# is the MSYS2 MinGW build (C ABI only), linked through an import library
# made from glslang.def and delay-loaded, so the OpenGL path starts even
# without its DLLs next to the executable.
option(XBOX_GPU_VULKAN "Build the Vulkan NV2A renderer" ON)
if(XBOX_GPU_VULKAN AND MSVC)
    enable_language(CXX)
    set(TP ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty)
    target_sources(xbox_gpu PRIVATE
        ${NV2A}/pgraph/vk/blit.c
        ${NV2A}/pgraph/vk/buffer.c
        ${NV2A}/pgraph/vk/command.c
        ${NV2A}/pgraph/vk/debug.c
        ${NV2A}/pgraph/vk/display.c
        ${NV2A}/pgraph/vk/draw.c
        ${NV2A}/pgraph/vk/glsl.c
        ${NV2A}/pgraph/vk/image.c
        ${NV2A}/pgraph/vk/instance.c
        ${NV2A}/pgraph/vk/renderer.c
        ${NV2A}/pgraph/vk/reports.c
        ${NV2A}/pgraph/vk/shaders.c
        ${NV2A}/pgraph/vk/surface-compute.c
        ${NV2A}/pgraph/vk/surface.c
        ${NV2A}/pgraph/vk/texture.c
        ${NV2A}/pgraph/vk/vertex.c
        ${TP}/volk/volk.c
        ${TP}/SPIRV-Reflect/spirv_reflect.c
        nv2a_recomp_vk.c
    )
    # VMA is C++; its own target keeps xbox_gpu's C-only flags off it.
    add_library(xbox_gpu_vma STATIC ${TP}/VulkanMemoryAllocator/vma.cpp)
    target_include_directories(xbox_gpu_vma PRIVATE
        ${TP}/Vulkan-Headers/include
        ${TP}/volk
        ${TP}/VulkanMemoryAllocator/include)
    target_compile_definitions(xbox_gpu_vma PRIVATE
        VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR
        VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=0)
    target_compile_options(xbox_gpu_vma PRIVATE /std:c++17 /W0)
    target_link_libraries(xbox_gpu PRIVATE xbox_gpu_vma)
    target_include_directories(xbox_gpu PRIVATE
        ${TP}/Vulkan-Headers/include
        ${TP}/glslang/include
        ${TP}/volk
        ${TP}/VulkanMemoryAllocator/include
        ${TP}/SPIRV-Reflect
    )
    target_compile_definitions(xbox_gpu PRIVATE
        XBOX_GPU_VULKAN=1 WIN32
        VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR
        VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=0)
    set_source_files_properties(
        ${TP}/volk/volk.c ${TP}/SPIRV-Reflect/spirv_reflect.c
        PROPERTIES COMPILE_OPTIONS "/W0")

    set(GLSLANG_IMPLIB ${CMAKE_CURRENT_BINARY_DIR}/glslang.lib)
    add_custom_command(OUTPUT ${GLSLANG_IMPLIB}
        COMMAND ${CMAKE_AR} /nologo /def:${TP}/glslang/glslang.def
                /machine:x64 /out:${GLSLANG_IMPLIB}
        DEPENDS ${TP}/glslang/glslang.def
        COMMENT "glslang import library")
    add_custom_target(xbox_gpu_glslang_implib DEPENDS ${GLSLANG_IMPLIB})
    add_dependencies(xbox_gpu xbox_gpu_glslang_implib)
    target_link_libraries(xbox_gpu PUBLIC ${GLSLANG_IMPLIB} delayimp)
    target_link_options(xbox_gpu INTERFACE /DELAYLOAD:glslang.dll)

    # DLLs the executable needs next to it for the Vulkan renderer
    # (projects copy them after the build, see projects/scda/CMakeLists.txt).
    file(GLOB XBOX_GPU_RUNTIME_DLLS ${TP}/glslang/bin/*.dll)
    set_property(TARGET xbox_gpu PROPERTY XBOX_RUNTIME_DLLS
                 ${XBOX_GPU_RUNTIME_DLLS})
endif()
