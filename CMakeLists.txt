cmake_minimum_required(VERSION 4.2)
project(lsmtree LANGUAGES CXX)

include(cmake/Format.cmake)

# Engine code lives in a library; tests (and a future CLI) link against it.
add_library(lsmtree_lib
        src/BloomFilter.cpp
        src/DB.cpp
        src/Manifest.cpp
        src/MemTable.cpp
        src/SSTable.cpp
        src/utils.cpp)
target_include_directories(lsmtree_lib PUBLIC inc)
target_compile_features(lsmtree_lib PUBLIC cxx_std_23)

enable_testing()

find_package(GTest CONFIG REQUIRED)

include(GoogleTest)
set(LSMTREE_GTEST_DISCOVERY_TIMEOUT 30)

function(add_lsmtree_test target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE lsmtree_lib GTest::gtest_main)
    gtest_discover_tests(${target} DISCOVERY_TIMEOUT ${LSMTREE_GTEST_DISCOVERY_TIMEOUT})
endfunction()

add_lsmtree_test(memtable_tests tests/memtable_tests.cpp)
add_lsmtree_test(sstable_tests tests/sstable_tests.cpp)
add_lsmtree_test(manifest_tests tests/manifest_tests.cpp)
add_lsmtree_test(db_tests tests/db_tests.cpp)
add_lsmtree_test(bloom_filter_tests tests/bloom_filter_tests.cpp)
add_lsmtree_test(utils_tests tests/utils_tests.cpp)
