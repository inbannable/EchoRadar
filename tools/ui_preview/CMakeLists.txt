add_executable(echoradar_ui_preview main.cpp
    ../../src/overlay/OverlayRenderer.cpp
    ../../src/overlay/DashboardView.cpp)
target_link_libraries(echoradar_ui_preview PRIVATE echoradar_ui echoradar_audio imgui_core)

target_compile_definitions(echoradar_ui_preview PRIVATE ECHORADAR_HEADLESS_RENDERER)
