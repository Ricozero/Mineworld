cmake_minimum_required(VERSION 3.10)

project(mineworld)

find_package(EnTT REQUIRED)
find_package(glm CONFIG REQUIRED)

if(MSVC)
	add_compile_options(/utf-8)
endif()

set(CMAKE_CXX_STANDARD 20)

add_subdirectory(src)