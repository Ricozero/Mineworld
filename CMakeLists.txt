cmake_minimum_required(VERSION 3.10)

project(mineworld)

set(CMAKE_CXX_STANDARD 17)

find_package(EnTT REQUIRED)

add_subdirectory(src)