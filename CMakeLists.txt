cmake_minimum_required(VERSION 3.21)

project(quiver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# quiver itself: a single header, consumed as an INTERFACE target.
add_library(quiver INTERFACE)
target_include_directories(quiver INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(quiver INTERFACE cxx_std_23)

# MSVC needs /std:c++latest for full C++23 library support (std::expected).
if(MSVC)
    target_compile_options(quiver INTERFACE /std:c++latest /Zc:__cplusplus)
endif()

function(quiver_executable name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE quiver)
    if(MSVC)
        target_compile_options(${name} PRIVATE /W4)
    else()
        target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

quiver_executable(quiver_tests tests/tests.cpp tests/tests_world.cpp tests/tests_bonds.cpp
                  tests/tests_queries.cpp tests/tests_reactive.cpp)
quiver_executable(quiver_example examples/gameplay.cpp)
quiver_executable(quiver_bench bench/bench.cpp)

enable_testing()
add_test(NAME quiver_tests COMMAND quiver_tests)
add_test(NAME quiver_example COMMAND quiver_example)

# EXPERIMENTAL: `import quiver;` via a named-module wrapper around the header.
# Off by default and never part of the test gate; if a toolchain rejects it,
# the header is the same library.
option(QUIVER_MODULE "Build the experimental C++23 module target" OFF)
if(QUIVER_MODULE AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
    add_library(quiver_module STATIC)
    target_sources(quiver_module PUBLIC FILE_SET CXX_MODULES FILES include/quiver.cppm)
    target_include_directories(quiver_module PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_features(quiver_module PUBLIC cxx_std_23)
    if(MSVC)
        target_compile_options(quiver_module PUBLIC /std:c++latest /Zc:__cplusplus)
    endif()
    add_executable(quiver_module_smoke tests/module_smoke.cpp)
    target_link_libraries(quiver_module_smoke PRIVATE quiver_module)
endif()
