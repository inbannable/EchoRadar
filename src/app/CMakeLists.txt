add_executable(EchoRadar
    main.cpp
    EchoRadarApp.cpp
    EchoRadarApp.h
)
target_include_directories(EchoRadar PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(EchoRadar
    PRIVATE
        echoradar_audio
        echoradar_dsp
        echoradar_events
        echoradar_features
        echoradar_localization
        echoradar_tracking
        echoradar_overlay
)

if(MSVC)
    target_compile_options(EchoRadar PRIVATE /W4)
else()
    target_compile_options(EchoRadar PRIVATE -Wall -Wextra -Wpedantic)
endif()
