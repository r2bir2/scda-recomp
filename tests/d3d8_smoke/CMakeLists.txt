# d3d8_smoke - offline smoke test for the pure xbox_d3d8 format layer.
# Compiles the real d3d8_resources.c + d3d8_swizzle.h with lightweight device
# accessor stubs so the format tables, predicates, swizzle classification and
# software conversions can be validated without a D3D11 device.

if(WIN32)
    add_executable(d3d8_smoke
        ${CMAKE_CURRENT_SOURCE_DIR}/test_main.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d/d3d8_resources.c
    )
    target_include_directories(d3d8_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d
    )
    target_link_libraries(d3d8_smoke PRIVATE platform d3d11 dxgi dxguid d3dcompiler)
    if(MSVC)
        target_compile_options(d3d8_smoke PRIVATE /W3)
    endif()
endif()
