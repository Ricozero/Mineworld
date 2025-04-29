cmake_minimum_required(VERSION 3.10)

set(APPLICATION_SOURCE main.cpp entt.cpp)

add_executable(mineworld ${APPLICATION_SOURCE})

target_link_libraries(mineworld	PRIVATE EnTT::EnTT)

set(EXECUTABLE_OUTPUT_PATH ../../bin)