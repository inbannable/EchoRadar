add_library(echoradar_events STATIC
    GunshotDetector.cpp
    GunshotDetector.h
    FootstepDetector.cpp
    FootstepDetector.h
    ../common/Types.h
)
target_include_directories(echoradar_events PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(echoradar_events PUBLIC echoradar_dsp)
