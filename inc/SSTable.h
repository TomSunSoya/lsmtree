#pragma once

#include "MemTable.h"

class SSTable
{
public:
    static void build(const MemTable &mt, const std::filesystem::path& path);
    static void cleanupOrphanedTemps(const std::filesystem::path& dir);

    explicit SSTable(std::filesystem::path path);

    bool get(std::string_view key, std::string &value) const;

private:
    std::filesystem::path path;
};
