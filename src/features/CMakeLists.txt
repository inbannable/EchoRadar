add_library(echoradar_features STATIC
    FeatureExtractor.cpp
    FeatureExtractor.h
    FeatureHistoryBuffer.cpp
    FeatureHistoryBuffer.h
    ../common/Types.h
)
target_include_directories(echoradar_features PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(echoradar_features PUBLIC echoradar_dsp)
