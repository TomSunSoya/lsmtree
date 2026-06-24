#pragma once
#include <cstdint>
#include <string>

enum class Type : uint8_t
{
    VALUE,
    TOMBSTONE
};

struct Entry
{
    Type type;
    std::string value;
};

enum class Result
{
    VALUE,
    TOMBSTONE,
    ABSENT
};

struct Record
{
    std::string key;
    Type type;
    std::string value;
};