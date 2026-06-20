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
