cmake_minimum_required(VERSION 3.10)

project(mineworld)

find_package(EnTT REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(spdlog REQUIRED)

if(MSVC)
	add_compile_options(/utf-8)
	add_compile_options(/std:c++23preview)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(src)