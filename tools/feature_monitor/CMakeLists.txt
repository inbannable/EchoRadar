add_executable(feature_monitor
    main.cpp
)
target_include_directories(feature_monitor PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(feature_monitor PRIVATE echoradar_audio echoradar_dsp echoradar_features)

if(MSVC)
    target_compile_options(feature_monitor PRIVATE /W4)
else()
    target_compile_options(feature_monitor PRIVATE -Wall -Wextra)
endif()
