set(ECHORADAR_RECOGNITION_SOURCES
    SoundRecognitionTypes.h
    PcmWav.h
    PcmWav.cpp
    LogMelExtractor.h
    LogMelExtractor.cpp
    EventPostProcessor.h
    EventPostProcessor.cpp
    ProbabilityModel.h
    OnnxProbabilityModel.h
    OnnxProbabilityModel.cpp
    ModelPackage.h
    ModelPackage.cpp
    SoundRecognizer.h
    SoundRecognizer.cpp
    Evaluation.h
    Evaluation.cpp
)

add_library(echoradar_recognition STATIC ${ECHORADAR_RECOGNITION_SOURCES})
target_include_directories(echoradar_recognition PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(echoradar_recognition PUBLIC echoradar_dsp echoradar_dataset)

if(ECHORADAR_ENABLE_ONNX)
    find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h
        HINTS "${ONNXRUNTIME_ROOT}/include" "${ONNXRUNTIME_ROOT}/build/native/include")
    find_library(ONNXRUNTIME_LIBRARY onnxruntime
        HINTS "${ONNXRUNTIME_ROOT}/lib" "${ONNXRUNTIME_ROOT}/runtimes/win-x64/native")
    find_file(ONNXRUNTIME_DLL onnxruntime.dll
        HINTS "${ONNXRUNTIME_ROOT}/bin" "${ONNXRUNTIME_ROOT}/runtimes/win-x64/native")
    if(NOT ONNXRUNTIME_INCLUDE_DIR OR NOT ONNXRUNTIME_LIBRARY OR (WIN32 AND NOT ONNXRUNTIME_DLL))
        message(FATAL_ERROR "ECHORADAR_ENABLE_ONNX requires ONNXRUNTIME_ROOT with include/ and lib/")
    endif()
    target_include_directories(echoradar_recognition PRIVATE ${ONNXRUNTIME_INCLUDE_DIR})
    target_link_libraries(echoradar_recognition PRIVATE ${ONNXRUNTIME_LIBRARY})
    target_compile_definitions(echoradar_recognition PRIVATE ECHORADAR_HAS_ONNXRUNTIME=1)
    set(ECHORADAR_ONNXRUNTIME_DLL "${ONNXRUNTIME_DLL}" CACHE INTERNAL "ONNX Runtime DLL")
endif()

if(MSVC)
    target_compile_options(echoradar_recognition PRIVATE /W4)
else()
    target_compile_options(echoradar_recognition PRIVATE -Wall -Wextra)
endif()
