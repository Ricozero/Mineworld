file(GLOB_RECURSE APPLICATION_SOURCE *.cpp)

add_executable(mineworld ${APPLICATION_SOURCE})

# Compile shaders
set(MINEWORLD_SHADER_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(MINEWORLD_SHADER_OUTPUT_DIR ${CMAKE_SOURCE_DIR}/bin/shaders)
set(MINEWORLD_VARYING_DEF ${MINEWORLD_SHADER_SOURCE_DIR}/varying.def.sc)

bgfx_compile_shaders(
    TYPE VERTEX
    SHADERS
    ${MINEWORLD_SHADER_SOURCE_DIR}/vs_color.sc
    ${MINEWORLD_SHADER_SOURCE_DIR}/vs_imgui.sc
    VARYING_DEF ${MINEWORLD_VARYING_DEF}
    OUTPUT_DIR ${MINEWORLD_SHADER_OUTPUT_DIR}
    OUT_FILES_VAR MINEWORLD_VERTEX_SHADERS
)

bgfx_compile_shaders(
    TYPE FRAGMENT
    SHADERS
    ${MINEWORLD_SHADER_SOURCE_DIR}/fs_color.sc
    ${MINEWORLD_SHADER_SOURCE_DIR}/fs_imgui.sc
    VARYING_DEF ${MINEWORLD_VARYING_DEF}
    OUTPUT_DIR ${MINEWORLD_SHADER_OUTPUT_DIR}
    OUT_FILES_VAR MINEWORLD_FRAGMENT_SHADERS
)

add_custom_target(mineworld_shaders DEPENDS ${MINEWORLD_VERTEX_SHADERS} ${MINEWORLD_FRAGMENT_SHADERS})
add_dependencies(mineworld mineworld_shaders)

# Copy config template to bin/ (always overwrite)
configure_file(${CMAKE_SOURCE_DIR}/config.ini ${CMAKE_SOURCE_DIR}/bin/config.ini COPYONLY)

# Generate FlatBuffers code
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

# Compile and link
target_link_libraries(mineworld PRIVATE
    EnTT::EnTT
    glm::glm
    spdlog::spdlog
    asio::asio
    kcp::kcp
    flatbuffers::flatbuffers
    glfw
    bgfx::bgfx
    imgui::imgui
    Tracy::TracyClient
)

target_compile_definitions(mineworld PRIVATE
    GLM_ENABLE_EXPERIMENTAL
    GLFW_EXPOSE_NATIVE_WIN32
)

if(WIN32)
    target_compile_definitions(mineworld PRIVATE
        _WIN32_WINNT=0x0A00
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
endif()

if(MSVC)
    target_compile_options(mineworld PRIVATE
        /utf-8
        /Zc:__cplusplus
        /Zc:preprocessor
    )
endif()
