add_executable(EchoRadar
    main.cpp
    EchoRadarApp.cpp
    EchoRadarApp.h
)
target_include_directories(EchoRadar PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(EchoRadar PRIVATE
    echoradar_audio
    echoradar_recognition
    echoradar_direction
    echoradar_settings
    echoradar_overlay
)

if(ECHORADAR_ENABLE_ONNX)
    add_custom_command(TARGET EchoRadar POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ECHORADAR_ONNXRUNTIME_DLL}"
            "$<TARGET_FILE_DIR:EchoRadar>/onnxruntime.dll")
endif()

if(MSVC)
    target_compile_options(EchoRadar PRIVATE /W4)
else()
    target_compile_options(EchoRadar PRIVATE -Wall -Wextra -Wpedantic)
endif()
