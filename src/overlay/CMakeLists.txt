add_library(echoradar_overlay STATIC
    HudOverlayRenderer.cpp
    HudOverlayRenderer.h
    OverlayRenderer.cpp
    OverlayRenderer.h
    ../common/Types.h
)
target_include_directories(echoradar_overlay PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(echoradar_overlay PUBLIC echoradar_tracking echoradar_settings)

if(WIN32)
    target_link_libraries(echoradar_overlay PUBLIC imgui_dx11 dcomp dxgi)
    target_compile_definitions(echoradar_overlay PUBLIC ECHORADAR_OVERLAY_ENABLED)
endif()
