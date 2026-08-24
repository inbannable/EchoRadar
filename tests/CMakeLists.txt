add_executable(echoradar_tests
    test_audio_ring_buffer.cpp
    test_audio_history_buffer.cpp
    test_surround_audio.cpp
    test_stft.cpp
    test_support.cpp
    test_pcm_wav.cpp
    test_recognition.cpp
    test_direction_scene.cpp
    test_app_settings.cpp
    test_v2_settings_ui.cpp
    test_radar.cpp
)
target_link_libraries(echoradar_tests PRIVATE
    echoradar_audio
    echoradar_direction
    echoradar_radar
    echoradar_settings
    echoradar_ui
    GTest::gtest_main
)
if(WIN32 AND ECHORADAR_ENABLE_ONNX)
    add_custom_command(TARGET echoradar_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ECHORADAR_ONNXRUNTIME_DLL}"
            "$<TARGET_FILE_DIR:echoradar_tests>/onnxruntime.dll")
endif()
include(GoogleTest)
gtest_discover_tests(echoradar_tests)
