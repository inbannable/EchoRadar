add_library(echoradar_audio STATIC
    AudioCapture.cpp
    AudioCapture.h
    ../common/Types.h
)
target_include_directories(echoradar_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(echoradar_audio PUBLIC miniaudio_iface)
