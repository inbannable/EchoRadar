add_library(echoradar_direction STATIC
    DirectionTypes.h
    DirectionModelPackage.h
    DirectionModelPackage.cpp
    DirectionOnnxEngine.h
    DirectionOnnxEngine.cpp
    DirectionSceneCoordinator.h
    DirectionSceneCoordinator.cpp
)
target_include_directories(echoradar_direction PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(echoradar_direction PUBLIC echoradar_recognition echoradar_support)

if(ECHORADAR_ENABLE_ONNX)
    target_link_libraries(echoradar_direction PRIVATE ONNXRuntime::ONNXRuntime)
endif()

if(MSVC)
    target_compile_options(echoradar_direction PRIVATE /W4)
else()
    target_compile_options(echoradar_direction PRIVATE -Wall -Wextra -Wpedantic)
endif()
