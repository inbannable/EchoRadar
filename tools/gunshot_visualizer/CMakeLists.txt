add_executable(gunshot_visualizer
    main.cpp
    DatasetStudioPanel.h
    DatasetStudioPanel.cpp
)
target_include_directories(gunshot_visualizer PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gunshot_visualizer PRIVATE
    echoradar_audio
    echoradar_dsp
    echoradar_features
    echoradar_detector
    echoradar_dataset
)

if(WIN32)
    target_link_libraries(gunshot_visualizer PRIVATE imgui_dx11 winmm)
endif()

if(MSVC)
    target_compile_options(gunshot_visualizer PRIVATE /W4)
else()
    target_compile_options(gunshot_visualizer PRIVATE -Wall -Wextra)
endif()
