cmake_minimum_required(VERSION 3.10)

file(GLOB_RECURSE APPLICATION_SOURCE *.cpp)

add_compile_definitions(GLM_ENABLE_EXPERIMENTAL)

add_executable(mineworld ${APPLICATION_SOURCE})

target_link_libraries(mineworld PRIVATE EnTT::EnTT)
target_link_libraries(mineworld PRIVATE glm::glm)
target_link_libraries(mineworld PRIVATE spdlog::spdlog_header_only)

set(EXECUTABLE_OUTPUT_PATH ../../bin)