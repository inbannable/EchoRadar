add_executable(sound_recognizer main.cpp)
target_include_directories(sound_recognizer PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(sound_recognizer PRIVATE echoradar_audio echoradar_recognition)
if(ECHORADAR_ENABLE_ONNX AND WIN32)
    add_custom_command(TARGET sound_recognizer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ECHORADAR_ONNXRUNTIME_DLL}" "$<TARGET_FILE_DIR:sound_recognizer>")
endif()
if(MSVC)
    target_compile_options(sound_recognizer PRIVATE /W4)
else()
    target_compile_options(sound_recognizer PRIVATE -Wall -Wextra)
endif()
