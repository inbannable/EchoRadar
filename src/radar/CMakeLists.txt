add_library(echoradar_radar STATIC
    RadarProcessor.cpp
    RadarProcessor.h
    RadarTypes.h
)
target_include_directories(echoradar_radar PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(echoradar_radar
    PUBLIC echoradar_audio
    PRIVATE kissfft::kissfft-float
)
if(MSVC)
    target_compile_options(echoradar_radar PRIVATE /W4)
else()
    target_compile_options(echoradar_radar PRIVATE -Wall -Wextra -Wpedantic)
endif()
