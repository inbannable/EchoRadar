# Minimal sandbox executable for testing
add_executable(echo_sandbox
    echo_sandbox.cpp
)

if(MSVC)
    target_compile_options(echo_sandbox PRIVATE /W4)
else()
    target_compile_options(echo_sandbox PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Full EchoRadar application (Milestone 1+)
add_executable(EchoRadar
    main.cpp
    EchoRadarApp.cpp
    EchoRadarApp.h
)
target_include_directories(EchoRadar PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(EchoRadar
    PRIVATE
        echoradar_audio
        echoradar_recognition
        echoradar_overlay
)

if(ECHORADAR_ENABLE_ONNX AND WIN32)
    add_custom_command(TARGET EchoRadar POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ECHORADAR_ONNXRUNTIME_DLL}" "$<TARGET_FILE_DIR:EchoRadar>")
endif()

if(MSVC)
    target_compile_options(EchoRadar PRIVATE /W4)
else()
    target_compile_options(EchoRadar PRIVATE -Wall -Wextra -Wpedantic)
endif()
