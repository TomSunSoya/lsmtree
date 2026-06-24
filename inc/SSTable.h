#pragma once

#include "MemTable.h"

class Cursor;

class SSTableWriter
{
public:
    explicit SSTableWriter(std::filesystem::path path);
    void add(const Record &);
    void finish();

private:
    std::filesystem::path path_, tempPath, parentDir;
    FdGuard dataFd;
};

class SSTable
{
public:
    static void build(const MemTable &mt, const std::filesystem::path& path);
    static void merge(const std::filesystem::path& path);
    static void cleanupOrphanedTemps(const std::filesystem::path& dir);

    explicit SSTable(std::filesystem::path path);

    Result get(std::string_view key, std::string &value) const;

private:
    std::filesystem::path path;
    friend class Cursor;

    static std::optional<Record> readOneRecord(std::ifstream &ifs);
};

class Cursor
{
public:
    explicit Cursor(std::filesystem::path  path);

    bool valid() const;
    const Record &current() const;
    void advance();

private:
    std::filesystem::path path;
    std::optional<Record> currentRecord{};
    std::ifstream ifs;
};
