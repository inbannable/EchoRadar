if(WIN32 AND ECHORADAR_BUILD_AUDIO_MONITOR)
    add_subdirectory(audio_monitor)
endif()

if(ECHORADAR_BUILD_STEAM_AUDIO_RENDERER)
    add_subdirectory(steam_audio_renderer)
endif()
