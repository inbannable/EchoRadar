add_library(echoradar_ui STATIC
    AppState.cpp
    AppState.h
)
target_include_directories(echoradar_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(echoradar_ui PUBLIC echoradar_settings)

if(MSVC)
    target_compile_options(echoradar_ui PRIVATE /W4)
else()
    target_compile_options(echoradar_ui PRIVATE -Wall -Wextra -Wpedantic)
endif()
