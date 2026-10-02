cmake_minimum_required(VERSION 3.24)
project(CarDX12 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(Car WIN32
    src/main.cpp
    src/Game.cpp
    src/Renderer.cpp
    src/Game.h
    src/Renderer.h
    src/GameTypes.h
)

target_compile_definitions(Car PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

target_link_libraries(Car PRIVATE
    d3d12
    dxgi
    d3dcompiler
)

if(MSVC)
    target_compile_options(Car PRIVATE /W4 /permissive- /EHsc)
endif()

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT Car)
