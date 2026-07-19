add_executable(asset_inventory
    main.cpp
)
target_include_directories(asset_inventory PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(asset_inventory PRIVATE echoradar_dataset)

if(MSVC)
    target_compile_options(asset_inventory PRIVATE /W4)
else()
    target_compile_options(asset_inventory PRIVATE -Wall -Wextra)
endif()
