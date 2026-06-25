add_executable(stft_monitor
    main.cpp
)
target_include_directories(stft_monitor PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(stft_monitor PRIVATE echoradar_audio echoradar_dsp)

if(MSVC)
    target_compile_options(stft_monitor PRIVATE /W4)
else()
    target_compile_options(stft_monitor PRIVATE -Wall -Wextra)
endif()
