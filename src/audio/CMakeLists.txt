add_library(echoradar_audio STATIC
    miniaudio_impl.cpp
    AudioCapture.cpp
    AudioCapture.h
    AudioDeviceManager.cpp
    AudioDeviceManager.h
    AudioDeviceInfo.h
    AudioHistoryBuffer.cpp
    AudioHistoryBuffer.h
    AudioRingBuffer.cpp
    AudioRingBuffer.h
    AudioStreamConsumer.h
    AudioTypes.h
    PcmWav.cpp
    PcmWav.h
)
target_include_directories(echoradar_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(echoradar_audio PUBLIC miniaudio_iface)

find_package(Threads REQUIRED)
target_link_libraries(echoradar_audio PUBLIC Threads::Threads)

if(APPLE)
    target_link_libraries(echoradar_audio PUBLIC
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework CoreFoundation")
elseif(UNIX AND NOT APPLE)
    find_package(ALSA QUIET)
    if(ALSA_FOUND)
        target_link_libraries(echoradar_audio PUBLIC ALSA::ALSA)
    endif()
    target_link_libraries(echoradar_audio PUBLIC dl m)
endif()
