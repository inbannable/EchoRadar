add_library(echoradar_tracking STATIC
    DirectionTracker.cpp
    DirectionTracker.h
    ../common/Types.h
)
target_include_directories(echoradar_tracking PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
