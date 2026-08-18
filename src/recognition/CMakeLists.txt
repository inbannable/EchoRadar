add_library(echoradar_recognition STATIC
    RecognitionTypes.h
    RecognitionProbabilityModel.h
    RecognitionModelPackage.h
    RecognitionModelPackage.cpp
    RecognitionRuntimeConfig.h
    StereoOnsetFeatureExtractor.h
    StereoOnsetFeatureExtractor.cpp
    OnnxRecognitionModel.h
    OnnxRecognitionModel.cpp
    SoundRecognizer.h
    SoundRecognizer.cpp
)
target_include_directories(echoradar_recognition PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(echoradar_recognition PUBLIC echoradar_dsp echoradar_support)

if(ECHORADAR_ENABLE_ONNX)
    target_link_libraries(echoradar_recognition PRIVATE ONNXRuntime::ONNXRuntime)
endif()

if(MSVC)
    target_compile_options(echoradar_recognition PRIVATE /W4)
else()
    target_compile_options(echoradar_recognition PRIVATE -Wall -Wextra -Wpedantic)
endif()
