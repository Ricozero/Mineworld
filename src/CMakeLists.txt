file(GLOB_RECURSE APPLICATION_SOURCE *.cpp)

add_executable(mineworld ${APPLICATION_SOURCE})

# 协议生成
set(MINEWORLD_FBS_SCHEMA ${CMAKE_CURRENT_SOURCE_DIR}/net_protocol.fbs)
set(MINEWORLD_FBS_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(MINEWORLD_FBS_GEN_HEADER ${MINEWORLD_FBS_GEN_DIR}/net_protocol_generated.h)

set(MINEWORLD_FLATC "")

if(TARGET flatbuffers::flatc)
    set(MINEWORLD_FLATC $<TARGET_FILE:flatbuffers::flatc>)
else()
    message(FATAL_ERROR "flatc not found")
endif()

add_custom_command(
    OUTPUT ${MINEWORLD_FBS_GEN_HEADER}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${MINEWORLD_FBS_GEN_DIR}
    COMMAND ${MINEWORLD_FLATC} --cpp --scoped-enums -o ${MINEWORLD_FBS_GEN_DIR} ${MINEWORLD_FBS_SCHEMA}
    DEPENDS ${MINEWORLD_FBS_SCHEMA}
    COMMENT "Generating FlatBuffers schema ${MINEWORLD_FBS_SCHEMA}"
    VERBATIM
)

add_custom_target(mineworld_fbs DEPENDS ${MINEWORLD_FBS_GEN_HEADER})
add_dependencies(mineworld mineworld_fbs)
target_include_directories(mineworld PRIVATE ${MINEWORLD_FBS_GEN_DIR})

# 编译链接
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
