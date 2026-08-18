add_executable(echoradar_tests
    test_audio_ring_buffer.cpp
    test_audio_history_buffer.cpp
    test_stft.cpp
    test_support.cpp
    test_pcm_wav.cpp
    test_recognition.cpp
    test_direction_scene.cpp
    test_app_settings.cpp
)
target_link_libraries(echoradar_tests PRIVATE
    echoradar_audio
    echoradar_direction
    echoradar_settings
    GTest::gtest_main
)
include(GoogleTest)
gtest_discover_tests(echoradar_tests)
