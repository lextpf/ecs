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

# ----------------------------------------------------------------------------
# Tests: Google Test, a single binary spanning every test translation unit.
# Links the ecs INTERFACE target, so the C++23 / std:c++latest requirements
# propagate here too.
# ----------------------------------------------------------------------------
find_package(GTest CONFIG REQUIRED)

add_executable(ecs_tests
    tests/tests.cpp tests/tests_world.cpp tests/tests_queries.cpp
    tests/tests_meta.cpp tests/tests_traits.cpp tests/tests_scheduler.cpp
    tests/tests_reactive.cpp)
target_link_libraries(ecs_tests PRIVATE ecs GTest::gtest GTest::gtest_main)
if(MSVC)
    target_compile_options(ecs_tests PRIVATE /W4)
endif()

# Run the gtest binary as ONE in-order ctest entry, not via gtest_discover_tests:
# the reflection suite shares a process-wide registry (registration is
# once-per-process, re-registration is a violation), so per-test process
# isolation would break those tests. The binary still emits full per-test gtest
# output -- run it directly (test.bat) or through this single ctest entry.
enable_testing()
add_test(NAME ecs_tests COMMAND ecs_tests)
