add_library(echoradar_support STATIC
    FileSha256.cpp
    FileSha256.h
    FlatJson.h
)
target_include_directories(echoradar_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

if(MSVC)
    target_compile_options(echoradar_support PRIVATE /W4)
else()
    target_compile_options(echoradar_support PRIVATE -Wall -Wextra -Wpedantic)
endif()
