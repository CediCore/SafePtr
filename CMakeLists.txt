cmake_minimum_required(VERSION 3.20)
project(SafePtr LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SAFEPTR_BUILD_TESTS "Build SafePtr test suite" ON)
option(SAFEPTR_BUILD_BENCH "Build SafePtr benchmark" ON)
option(SAFEPTR_BUILD_DOCS "Generate Doxygen documentation" ON)

# Header-only library
add_library(safeptr INTERFACE)
target_include_directories(safeptr INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ============================
# Tests
# ============================
if (SAFEPTR_BUILD_TESTS)
    enable_testing()

    include(cmake/FetchGTest.cmake)

    add_executable(test_safeptr tests/test_safeptr.cpp)
    target_link_libraries(test_safeptr PRIVATE safeptr GTest::gtest_main)
    add_test(NAME safeptr_tests COMMAND test_safeptr)
endif()

# ============================
# Benchmark
# ============================
if (SAFEPTR_BUILD_BENCH)
    add_executable(bench_safeptr benchmark/bench_safeptr.cpp)
    target_link_libraries(bench_safeptr PRIVATE safeptr)
endif()

# ============================
# Documentation (Doxygen)
# ============================
if (SAFEPTR_BUILD_DOCS)
    find_package(Doxygen REQUIRED)
    add_custom_target(
        docs
        COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/docs/Doxyfile
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
endif()
