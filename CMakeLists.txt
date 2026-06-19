cmake_minimum_required(VERSION 4.2)
project(lsmtree)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Engine code lives in a library; tests (and a future CLI) link against it.
add_library(lsmtree_lib
        src/MemTable.cpp
        src/SSTable.cpp
        inc/SSTable.h
        src/DB.cpp
        inc/DB.h)
target_include_directories(lsmtree_lib PUBLIC inc)

enable_testing()

find_package(GTest CONFIG REQUIRED)

add_executable(memtable_tests tests/memtable_tests.cpp)
target_link_libraries(memtable_tests PRIVATE lsmtree_lib GTest::gtest_main)

add_executable(sstable_tests tests/sstable_tests.cpp)
target_link_libraries(sstable_tests PRIVATE lsmtree_lib GTest::gtest_main)

add_executable(db_tests tests/db_tests.cpp
        inc/utils.h)
target_link_libraries(db_tests PRIVATE lsmtree_lib GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(memtable_tests)
gtest_discover_tests(sstable_tests)
gtest_discover_tests(db_tests)
