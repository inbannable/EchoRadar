add_executable(dataset_recorder
    main.cpp
)
target_include_directories(dataset_recorder PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(dataset_recorder
    PRIVATE
        echoradar_audio
        echoradar_dsp
        echoradar_features
        echoradar_detector
)

if(WIN32)
    target_link_libraries(dataset_recorder PRIVATE imgui_dx11 winmm)
endif()

if(MSVC)
    target_compile_options(dataset_recorder PRIVATE /W4)
else()
    target_compile_options(dataset_recorder PRIVATE -Wall -Wextra)
endif()
