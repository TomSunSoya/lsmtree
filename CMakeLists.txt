cmake_minimum_required(VERSION 4.2)
project(lsmtree)

set(CMAKE_CXX_STANDARD 23)

add_executable(lsmtree main.cpp
        src/MemTable.cpp
        inc/MemTable.h)
