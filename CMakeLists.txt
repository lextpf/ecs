cmake_minimum_required(VERSION 3.21)

project(ecs LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ecs itself: a single header, consumed as an INTERFACE target.
add_library(ecs INTERFACE)
target_include_directories(ecs INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(ecs INTERFACE cxx_std_23)

# MSVC needs /std:c++latest for full C++23 library support (std::expected).
if(MSVC)
    target_compile_options(ecs INTERFACE /std:c++latest /Zc:__cplusplus)
endif()

function(ecs_executable name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE ecs)
    if(MSVC)
        target_compile_options(${name} PRIVATE /W4)
    else()
        target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

ecs_executable(ecs_tests tests/tests.cpp tests/tests_world.cpp tests/tests_bonds.cpp
               tests/tests_queries.cpp tests/tests_reactive.cpp tests/tests_meta.cpp
               tests/tests_traits.cpp)
ecs_executable(ecs_example examples/gameplay.cpp)
ecs_executable(ecs_bench bench/bench.cpp)

enable_testing()
add_test(NAME ecs_tests COMMAND ecs_tests)
add_test(NAME ecs_example COMMAND ecs_example)
