add_library(echoradar_detector STATIC
    GunshotEventDetector.cpp
    GunshotEventDetector.h
    GunshotEvent.h
)
target_include_directories(echoradar_detector PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
