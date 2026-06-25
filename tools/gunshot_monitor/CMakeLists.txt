add_executable(gunshot_monitor
    main.cpp
)
target_include_directories(gunshot_monitor PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gunshot_monitor PRIVATE echoradar_audio echoradar_dsp echoradar_features echoradar_detector)

if(MSVC)
    target_compile_options(gunshot_monitor PRIVATE /W4)
else()
    target_compile_options(gunshot_monitor PRIVATE -Wall -Wextra)
endif()
