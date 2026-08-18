add_library(echoradar_dsp STATIC
    WindowFunctions.cpp
    WindowFunctions.h
    STFTProcessor.cpp
    STFTProcessor.h
)
target_include_directories(echoradar_dsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(echoradar_dsp PUBLIC kissfft::kissfft-float)
