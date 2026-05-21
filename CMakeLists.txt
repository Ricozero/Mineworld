cmake_minimum_required(VERSION 3.10)
project(mineworld)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
foreach(OUTPUT_CONFIG Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${OUTPUT_CONFIG} OUTPUT_CONFIG_UPPER)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUT_CONFIG_UPPER} ${CMAKE_SOURCE_DIR}/bin)
endforeach()

find_package(EnTT REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(spdlog REQUIRED)
find_package(asio REQUIRED)
find_package(kcp CONFIG REQUIRED)
find_package(flatbuffers CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(bgfx CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

add_subdirectory(src)
