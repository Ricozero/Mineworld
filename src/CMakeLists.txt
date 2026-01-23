cmake_minimum_required(VERSION 3.10)

file(GLOB_RECURSE APPLICATION_SOURCE *.cpp)

add_executable(mineworld ${APPLICATION_SOURCE})

target_link_libraries(mineworld	PRIVATE EnTT::EnTT)
target_link_libraries(mineworld PRIVATE glm::glm)

set(EXECUTABLE_OUTPUT_PATH ../../bin)