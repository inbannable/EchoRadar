add_library(echoradar_dataset STATIC
    DatasetTypes.h
    DatasetJson.h
    DatasetWavStats.h
    DatasetManager.h
    DatasetManager.cpp
)
target_include_directories(echoradar_dataset PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
