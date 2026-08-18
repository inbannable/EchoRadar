add_library(echoradar_localization STATIC
    LocalizationTypes.cpp
    LocalizationTypes.h
    CalibrationController.cpp
    CalibrationController.h
    KNNDirectionEstimator.cpp
    KNNDirectionEstimator.h
    StereoDirectionEstimator.cpp
    StereoDirectionEstimator.h
    DirectionModelPackage.cpp
    DirectionModelPackage.h
    DirectionOnnxEngine.cpp
    DirectionOnnxEngine.h
    DirectionSceneCoordinator.cpp
    DirectionSceneCoordinator.h
    ../common/Types.h
)
target_include_directories(echoradar_localization PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(echoradar_localization PUBLIC echoradar_features echoradar_recognition)

if(ECHORADAR_ENABLE_ONNX)
    find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h
        HINTS "${ONNXRUNTIME_ROOT}/include" "${ONNXRUNTIME_ROOT}/build/native/include")
    find_library(ONNXRUNTIME_LIBRARY onnxruntime
        HINTS "${ONNXRUNTIME_ROOT}/lib" "${ONNXRUNTIME_ROOT}/runtimes/win-x64/native")
    if(NOT ONNXRUNTIME_INCLUDE_DIR OR NOT ONNXRUNTIME_LIBRARY)
        message(FATAL_ERROR "Direction inference requires ONNXRUNTIME_ROOT with include/ and lib/")
    endif()
    target_include_directories(echoradar_localization PRIVATE ${ONNXRUNTIME_INCLUDE_DIR})
    target_link_libraries(echoradar_localization PRIVATE ${ONNXRUNTIME_LIBRARY})
    target_compile_definitions(echoradar_localization PRIVATE ECHORADAR_HAS_ONNXRUNTIME=1)
endif()

if(MSVC)
    target_compile_options(echoradar_localization PRIVATE /W4)
else()
    target_compile_options(echoradar_localization PRIVATE -Wall -Wextra)
endif()
