cmake_minimum_required(VERSION 4.2)
project(lsmtree)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Engine code lives in a library; tests (and a future CLI) link against it.
add_library(lsmtree_lib
        src/MemTable.cpp)
target_include_directories(lsmtree_lib PUBLIC inc)

enable_testing()

find_package(GTest CONFIG REQUIRED)

add_executable(memtable_tests tests/memtable_tests.cpp)
target_link_libraries(memtable_tests PRIVATE lsmtree_lib GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(memtable_tests)
