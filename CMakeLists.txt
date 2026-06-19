cmake_minimum_required(VERSION 3.20)
project(EchoRadar VERSION 0.1.0 LANGUAGES CXX)

# ── C++20 ──────────────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Build options ──────────────────────────────────────────────────────────────
option(ECHORADAR_BUILD_TESTS "Build unit tests"  ON)
option(ECHORADAR_BUILD_TOOLS "Build helper tools" ON)

# ── FetchContent ───────────────────────────────────────────────────────────────
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# KissFFT  (lightweight FFT, no external install required)
FetchContent_Declare(
    kissfft
    GIT_REPOSITORY https://github.com/mborgerding/kissfft.git
    GIT_TAG        131.1.0
    GIT_SHALLOW    TRUE
)
set(KISSFFT_TEST     OFF CACHE BOOL "" FORCE)
set(KISSFFT_TOOLS    OFF CACHE BOOL "" FORCE)
set(KISSFFT_STATIC   ON  CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(kissfft)

# miniaudio  (single-header audio I/O)
FetchContent_Declare(
    miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG        0.11.21
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(miniaudio)

# Interface target so every module can do: target_link_libraries(... miniaudio_iface)
add_library(miniaudio_iface INTERFACE)
target_include_directories(miniaudio_iface INTERFACE ${miniaudio_SOURCE_DIR})

# GoogleTest
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
    GIT_SHALLOW    TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Dear ImGui + DirectX 11 backend  (Windows only)
if(WIN32)
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.90.8
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(imgui)

    # Build ImGui as a static library with DX11 backend
    add_library(imgui_dx11 STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    )
    target_include_directories(imgui_dx11
        PUBLIC
            ${imgui_SOURCE_DIR}
            ${imgui_SOURCE_DIR}/backends
    )
    target_link_libraries(imgui_dx11 PUBLIC d3d11 d3dcompiler dxgi)
endif()

# ── Subdirectories ─────────────────────────────────────────────────────────────
add_subdirectory(src)

if(ECHORADAR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(ECHORADAR_BUILD_TOOLS)
    add_subdirectory(tools)
endif()
