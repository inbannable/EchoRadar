add_library(echoradar_audio STATIC
    miniaudio_impl.cpp          # defines MINIAUDIO_IMPLEMENTATION (exactly once)
    AudioCapture.cpp
    AudioCapture.h
    AudioDeviceManager.cpp
    AudioDeviceManager.h
    AudioDeviceInfo.h
    AudioRingBuffer.cpp
    AudioRingBuffer.h
    ../common/Types.h
)
target_include_directories(echoradar_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(echoradar_audio PUBLIC miniaudio_iface)

# Platform-specific dependencies required by the miniaudio implementation.
find_package(Threads REQUIRED)
target_link_libraries(echoradar_audio PUBLIC Threads::Threads)

if(APPLE)
    target_link_libraries(echoradar_audio PUBLIC
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework CoreFoundation"
    )
elseif(UNIX AND NOT APPLE)
    # Linux: try ALSA; PulseAudio is handled via miniaudio's built-in support.
    find_package(ALSA QUIET)
    if(ALSA_FOUND)
        target_link_libraries(echoradar_audio PUBLIC ALSA::ALSA)
    endif()
    target_link_libraries(echoradar_audio PUBLIC dl m)
endif()
