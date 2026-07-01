cmake_minimum_required(VERSION 4.2)
project(lsmtree)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Engine code lives in a library; tests (and a future CLI) link against it.
add_library(lsmtree_lib
        src/MemTable.cpp
        src/SSTable.cpp
        src/utils.cpp
        src/Manifest.cpp
        inc/SSTable.h
        inc/Manifest.h
        src/DB.cpp
        inc/DB.h
        src/BloomFilter.cpp
        inc/BloomFilter.h)
target_include_directories(lsmtree_lib PUBLIC inc)

enable_testing()

find_package(GTest CONFIG REQUIRED)

add_executable(memtable_tests tests/memtable_tests.cpp)
target_link_libraries(memtable_tests PRIVATE lsmtree_lib GTest::gtest_main)

add_executable(sstable_tests tests/sstable_tests.cpp)
target_link_libraries(sstable_tests PRIVATE lsmtree_lib GTest::gtest_main)

add_executable(manifest_tests tests/manifest_tests.cpp)
target_link_libraries(manifest_tests PRIVATE lsmtree_lib GTest::gtest_main)

add_executable(db_tests tests/db_tests.cpp)
target_link_libraries(db_tests PRIVATE lsmtree_lib GTest::gtest_main)

add_executable(bloom_filter_tests tests/bloom_filter_tests.cpp)
target_link_libraries(bloom_filter_tests PRIVATE lsmtree_lib GTest::gtest_main)

add_executable(utils_tests tests/utils_tests.cpp)
target_link_libraries(utils_tests PRIVATE lsmtree_lib GTest::gtest_main)

include(GoogleTest)
set(LSMTREE_GTEST_DISCOVERY_TIMEOUT 30)
gtest_discover_tests(memtable_tests DISCOVERY_TIMEOUT ${LSMTREE_GTEST_DISCOVERY_TIMEOUT})
gtest_discover_tests(sstable_tests DISCOVERY_TIMEOUT ${LSMTREE_GTEST_DISCOVERY_TIMEOUT})
gtest_discover_tests(manifest_tests DISCOVERY_TIMEOUT ${LSMTREE_GTEST_DISCOVERY_TIMEOUT})
gtest_discover_tests(db_tests DISCOVERY_TIMEOUT ${LSMTREE_GTEST_DISCOVERY_TIMEOUT})
gtest_discover_tests(bloom_filter_tests DISCOVERY_TIMEOUT ${LSMTREE_GTEST_DISCOVERY_TIMEOUT})
gtest_discover_tests(utils_tests DISCOVERY_TIMEOUT ${LSMTREE_GTEST_DISCOVERY_TIMEOUT})
