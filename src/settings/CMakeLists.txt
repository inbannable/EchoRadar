add_library(echoradar_settings STATIC
    AppSettings.cpp
    AppSettings.h
)
target_include_directories(echoradar_settings PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(echoradar_settings PUBLIC echoradar_localization echoradar_dataset)

if(MSVC)
    target_compile_options(echoradar_settings PRIVATE /W4)
else()
    target_compile_options(echoradar_settings PRIVATE -Wall -Wextra -Wpedantic)
endif()
