add_executable(audio_monitor
    main.cpp
)
target_include_directories(audio_monitor PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(audio_monitor PRIVATE echoradar_audio)

if(MSVC)
    target_compile_options(audio_monitor PRIVATE /W4)
else()
    target_compile_options(audio_monitor PRIVATE -Wall -Wextra)
endif()
