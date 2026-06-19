add_library(echoradar_dsp STATIC
    RingBuffer.cpp
    RingBuffer.h
    STFTProcessor.cpp
    STFTProcessor.h
    ../common/Types.h
)
target_include_directories(echoradar_dsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(echoradar_dsp PUBLIC kissfft::kissfft-float)
