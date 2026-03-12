file(GLOB_RECURSE APPLICATION_SOURCE *.cpp)

add_executable(mineworld ${APPLICATION_SOURCE})

target_link_libraries(mineworld PRIVATE
    EnTT::EnTT
    glm::glm
    spdlog::spdlog
    asio::asio
    kcp::kcp
    flatbuffers::flatbuffers
)

target_compile_definitions(mineworld PRIVATE
    GLM_ENABLE_EXPERIMENTAL
)

if(WIN32)
    target_compile_definitions(mineworld PRIVATE
        _WIN32_WINNT=0x0A00
        WIN32_LEAN_AND_MEAN
    )
endif()

if(MSVC)
    target_compile_options(mineworld PRIVATE
        /utf-8
    )
endif()
