file(GLOB_RECURSE MINEWORLD_CORE_SOURCE CONFIGURE_DEPENDS *.cpp)
list(REMOVE_ITEM MINEWORLD_CORE_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp)

add_library(mineworld_core STATIC ${MINEWORLD_CORE_SOURCE})
add_executable(mineworld main.cpp)
target_link_libraries(mineworld PRIVATE mineworld_core)

# Copy config template to bin
configure_file(${CMAKE_SOURCE_DIR}/config.ini ${CMAKE_SOURCE_DIR}/bin/config.ini COPYONLY)

# Compile shaders
set(MINEWORLD_SHADER_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(MINEWORLD_SHADER_OUTPUT_DIR ${CMAKE_SOURCE_DIR}/bin/shaders)
set(MINEWORLD_SHADER_PROGRAMS unlit imgui chunk)
set(MINEWORLD_SHADER_BINARIES "")
set(MINEWORLD_VARYING_DEF ${MINEWORLD_SHADER_SOURCE_DIR}/varying.def.sc)

foreach(program ${MINEWORLD_SHADER_PROGRAMS})
    bgfx_compile_shaders(
        TYPE VERTEX
        SHADERS ${MINEWORLD_SHADER_SOURCE_DIR}/vs_${program}.sc
        VARYING_DEF ${MINEWORLD_VARYING_DEF}
        OUTPUT_DIR ${MINEWORLD_SHADER_OUTPUT_DIR}
        OUT_FILES_VAR MINEWORLD_VERTEX_SHADERS
    )

    bgfx_compile_shaders(
        TYPE FRAGMENT
        SHADERS ${MINEWORLD_SHADER_SOURCE_DIR}/fs_${program}.sc
        VARYING_DEF ${MINEWORLD_VARYING_DEF}
        OUTPUT_DIR ${MINEWORLD_SHADER_OUTPUT_DIR}
        OUT_FILES_VAR MINEWORLD_FRAGMENT_SHADERS
    )

    list(APPEND MINEWORLD_SHADER_BINARIES ${MINEWORLD_VERTEX_SHADERS} ${MINEWORLD_FRAGMENT_SHADERS})
endforeach()

add_custom_target(mineworld_shaders DEPENDS ${MINEWORLD_SHADER_BINARIES})
add_dependencies(mineworld mineworld_shaders)

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
add_dependencies(mineworld_core mineworld_fbs)
target_include_directories(mineworld_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MINEWORLD_FBS_GEN_DIR}
)

# Compile and link
target_link_libraries(mineworld_core PUBLIC
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
    lz4::lz4
)

target_compile_definitions(mineworld_core PUBLIC
    GLM_ENABLE_EXPERIMENTAL
    GLFW_EXPOSE_NATIVE_WIN32
)

if(WIN32)
    target_compile_definitions(mineworld_core PUBLIC
        _WIN32_WINNT=0x0A00
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
endif()

if(MSVC)
    target_compile_options(mineworld_core PUBLIC
        /utf-8
        /Zc:__cplusplus
        /Zc:preprocessor
    )
endif()
