add_executable(sound_eval main.cpp)
target_include_directories(sound_eval PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(sound_eval PRIVATE echoradar_recognition)
if(ECHORADAR_ENABLE_ONNX AND WIN32)
    add_custom_command(TARGET sound_eval POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ECHORADAR_ONNXRUNTIME_DLL}" "$<TARGET_FILE_DIR:sound_eval>")
endif()
if(MSVC)
    target_compile_options(sound_eval PRIVATE /W4)
else()
    target_compile_options(sound_eval PRIVATE -Wall -Wextra)
endif()
