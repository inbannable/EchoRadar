cmake_minimum_required(VERSION 3.20)
project(EchoRadar VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ECHORADAR_BUILD_APP "Build the Windows EchoRadar application" ON)
option(ECHORADAR_BUILD_AUDIO_MONITOR "Build the Windows loopback audio monitor" ON)
option(ECHORADAR_BUILD_TESTS "Build automated tests" OFF)
option(ECHORADAR_ENABLE_ONNX "Enable ONNX Runtime inference" OFF)
option(ECHORADAR_BUILD_STEAM_AUDIO_RENDERER
       "Build the optional offline Steam Audio v4.8.1 renderer" OFF)

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(
    kissfft
    GIT_REPOSITORY https://github.com/mborgerding/kissfft.git
    GIT_TAG 131.1.0
    GIT_SHALLOW TRUE
)
set(KISSFFT_TEST OFF CACHE BOOL "" FORCE)
set(KISSFFT_TOOLS OFF CACHE BOOL "" FORCE)
set(KISSFFT_STATIC ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(kissfft)

FetchContent_Declare(
    miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG 0.11.21
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(miniaudio)
add_library(miniaudio_iface INTERFACE)
target_include_directories(miniaudio_iface INTERFACE ${miniaudio_SOURCE_DIR})

if(ECHORADAR_ENABLE_ONNX)
    find_path(ECHORADAR_ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h
        HINTS "${ONNXRUNTIME_ROOT}/include"
              "${ONNXRUNTIME_ROOT}/build/native/include")
    find_library(ECHORADAR_ONNXRUNTIME_LIBRARY onnxruntime
        HINTS "${ONNXRUNTIME_ROOT}/lib"
              "${ONNXRUNTIME_ROOT}/runtimes/win-x64/native")
    if(WIN32)
        find_file(ECHORADAR_ONNXRUNTIME_DLL onnxruntime.dll
            HINTS "${ONNXRUNTIME_ROOT}/bin"
                  "${ONNXRUNTIME_ROOT}/runtimes/win-x64/native")
    endif()
    if(NOT ECHORADAR_ONNXRUNTIME_INCLUDE_DIR OR
       NOT ECHORADAR_ONNXRUNTIME_LIBRARY OR
       (WIN32 AND NOT ECHORADAR_ONNXRUNTIME_DLL))
        message(FATAL_ERROR
            "ECHORADAR_ENABLE_ONNX requires ONNXRUNTIME_ROOT with headers, library, "
            "and on Windows runtimes/win-x64/native/onnxruntime.dll")
    endif()
    add_library(ONNXRuntime::ONNXRuntime UNKNOWN IMPORTED)
    set_target_properties(ONNXRuntime::ONNXRuntime PROPERTIES
        IMPORTED_LOCATION "${ECHORADAR_ONNXRUNTIME_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${ECHORADAR_ONNXRUNTIME_INCLUDE_DIR}"
        INTERFACE_COMPILE_DEFINITIONS ECHORADAR_HAS_ONNXRUNTIME=1)
endif()

if(ECHORADAR_BUILD_TESTS)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
        GIT_SHALLOW TRUE
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

if(WIN32 AND ECHORADAR_BUILD_APP)
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.90.8
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(imgui)
    add_library(imgui_dx11 STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp)
    target_include_directories(imgui_dx11 PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends)
    target_link_libraries(imgui_dx11 PUBLIC d3d11 d3dcompiler dxgi)
elseif(ECHORADAR_BUILD_APP AND NOT WIN32)
    message(STATUS "EchoRadar application is Windows-only; building supported libraries instead")
endif()

add_subdirectory(src)

if(ECHORADAR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if((WIN32 AND ECHORADAR_BUILD_AUDIO_MONITOR) OR
   ECHORADAR_BUILD_STEAM_AUDIO_RENDERER)
    add_subdirectory(tools)
endif()
