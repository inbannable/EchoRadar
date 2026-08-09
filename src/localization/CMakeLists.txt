add_library(echoradar_localization STATIC
    LocalizationTypes.cpp
    LocalizationTypes.h
    CalibrationController.cpp
    CalibrationController.h
    KNNDirectionEstimator.cpp
    KNNDirectionEstimator.h
    StereoDirectionEstimator.cpp
    StereoDirectionEstimator.h
    ../common/Types.h
)
target_include_directories(echoradar_localization PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(echoradar_localization PUBLIC echoradar_features)
