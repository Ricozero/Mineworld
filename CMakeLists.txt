cmake_minimum_required(VERSION 3.10)
project(mineworld)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

find_package(EnTT REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(spdlog REQUIRED)
find_package(asio REQUIRED)
find_package(kcp CONFIG REQUIRED)
find_package(flatbuffers CONFIG REQUIRED)

add_subdirectory(src)