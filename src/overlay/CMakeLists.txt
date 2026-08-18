add_library(echoradar_overlay STATIC
    HudOverlayRenderer.cpp
    HudOverlayRenderer.h
    OverlayRenderer.cpp
    OverlayRenderer.h
)
target_include_directories(echoradar_overlay PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(echoradar_overlay PUBLIC
    echoradar_audio
    echoradar_recognition
    echoradar_direction
    echoradar_settings
    imgui_dx11
    dcomp
    dxgi
    winmm
)
target_compile_definitions(echoradar_overlay PUBLIC ECHORADAR_OVERLAY_ENABLED)

if(MSVC)
    target_compile_options(echoradar_overlay PRIVATE /W4)
else()
    target_compile_options(echoradar_overlay PRIVATE -Wall -Wextra -Wpedantic)
endif()
