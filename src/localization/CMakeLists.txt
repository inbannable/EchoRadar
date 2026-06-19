add_library(echoradar_localization STATIC
    KNNDirectionEstimator.cpp
    KNNDirectionEstimator.h
    ../common/Types.h
)
target_include_directories(echoradar_localization PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(echoradar_localization PUBLIC echoradar_features)
