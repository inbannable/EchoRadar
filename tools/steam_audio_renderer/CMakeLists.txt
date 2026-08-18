find_path(STEAMAUDIO_INCLUDE_DIR phonon.h
    HINTS "${STEAMAUDIO_ROOT}/include" "${STEAMAUDIO_ROOT}/core/src/core")
find_library(STEAMAUDIO_LIBRARY phonon
    HINTS "${STEAMAUDIO_ROOT}/lib/windows-x64" "${STEAMAUDIO_ROOT}/lib")
find_file(STEAMAUDIO_DLL phonon.dll
    HINTS "${STEAMAUDIO_ROOT}/lib/windows-x64" "${STEAMAUDIO_ROOT}/bin/windows-x64"
          "${STEAMAUDIO_ROOT}/lib" "${STEAMAUDIO_ROOT}/bin")
if(NOT STEAMAUDIO_INCLUDE_DIR OR NOT STEAMAUDIO_LIBRARY OR (WIN32 AND NOT STEAMAUDIO_DLL))
    message(FATAL_ERROR
        "ECHORADAR_BUILD_STEAM_AUDIO_RENDERER requires STEAMAUDIO_ROOT for official v4.8.1")
endif()

add_executable(echoradar_steam_audio_renderer main.cpp)
target_include_directories(echoradar_steam_audio_renderer PRIVATE ${STEAMAUDIO_INCLUDE_DIR})
target_link_libraries(echoradar_steam_audio_renderer PRIVATE echoradar_audio ${STEAMAUDIO_LIBRARY})
if(WIN32)
    add_custom_command(TARGET echoradar_steam_audio_renderer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${STEAMAUDIO_DLL}" "$<TARGET_FILE_DIR:echoradar_steam_audio_renderer>")
endif()
if(MSVC)
    target_compile_options(echoradar_steam_audio_renderer PRIVATE /W4)
else()
    target_compile_options(echoradar_steam_audio_renderer PRIVATE -Wall -Wextra)
endif()
