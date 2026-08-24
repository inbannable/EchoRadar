add_executable(EchoRadarV2
    main.cpp
    EchoRadarApp.cpp
    EchoRadarApp.h
    SessionLog.cpp
    SessionLog.h
)
target_include_directories(EchoRadarV2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(EchoRadarV2 PRIVATE
    echoradar_audio
    echoradar_recognition
    echoradar_radar
    echoradar_settings
    echoradar_ui
    echoradar_overlay
)

if(ECHORADAR_ENABLE_ONNX)
    add_custom_command(TARGET EchoRadarV2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ECHORADAR_ONNXRUNTIME_DLL}"
            "$<TARGET_FILE_DIR:EchoRadarV2>/onnxruntime.dll")
endif()

if(MSVC)
    target_compile_options(EchoRadarV2 PRIVATE /W4)
else()
    target_compile_options(EchoRadarV2 PRIVATE -Wall -Wextra -Wpedantic)
endif()
