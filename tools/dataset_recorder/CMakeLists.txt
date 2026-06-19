add_executable(dataset_recorder
    main.cpp
)
target_include_directories(dataset_recorder PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(dataset_recorder
    PRIVATE
        echoradar_audio
        echoradar_dsp
)
