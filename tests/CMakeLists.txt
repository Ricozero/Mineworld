add_executable(mineworld_tests
    unit_tests.cpp
    network_tests.cpp
)
add_dependencies(mineworld_tests mineworld_fbs)
target_link_libraries(mineworld_tests PRIVATE mineworld_core GTest::gtest_main)
set_target_properties(mineworld_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

foreach(config Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${config} config_upper)
    set_target_properties(mineworld_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${config_upper} ${CMAKE_CURRENT_BINARY_DIR})
endforeach()

include(GoogleTest)
gtest_discover_tests(mineworld_tests
    DISCOVERY_TIMEOUT 30
    PROPERTIES TIMEOUT 120
)
