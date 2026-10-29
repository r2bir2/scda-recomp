# Shared FMV player: Media Foundation decode onto a D3D8 texture.
add_library(xbox_video STATIC
    video_player.c
    video_pump.c
)
if(WIN32)
    target_sources(xbox_video PRIVATE fb_present.c)
else()
    # SDL2 window, input and EGL presenting (ARM port, docs Update 677)
    find_package(SDL2 REQUIRED)
    target_sources(xbox_video PRIVATE fb_present_sdl.c)
    target_link_libraries(xbox_video PUBLIC SDL2::SDL2 platform)
endif()
target_include_directories(xbox_video PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(xbox_video PUBLIC xbox_d3d8)
if(WIN32)
    target_link_libraries(xbox_video PUBLIC mfplat mfreadwrite mfuuid mf ole32)
endif()
