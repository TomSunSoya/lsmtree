#include "DB.h"

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <format>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <ranges>

#include "SSTable.h"

namespace
{
    namespace fs = std::filesystem;

    constexpr std::string_view kWalPrefix = "wal_";
    constexpr std::string_view kWalSuffix = ".wal";
    constexpr std::string_view kSSTablePrefix = "sst_";
    constexpr std::string_view kSSTableSuffix = ".sst";

    fs::path walPath(const fs::path& dataDir, const uint64_t fileNumber)
    {
        return dataDir / "wal" / std::format("{}{}{}", kWalPrefix, fileNumber, kWalSuffix);
    }

    fs::path sstablePath(const fs::path& dataDir, const uint64_t fileNumber)
    {
        return dataDir / "sstable" / std::format("{}{}{}", kSSTablePrefix, fileNumber, kSSTableSuffix);
    }

    void removeFile(const fs::path& path, const char* message)
    {
        if (::remove(path.c_str()))
        {
            const auto err = errno;
            throw std::system_error(err, std::system_category(), message);
        }
    }

    void cleanupOrphanedWAL(const std::filesystem::path& dir, const uint64_t currentFileNumber)
    {
        if (!fs::exists(dir) || !fs::is_directory(dir))
            return;

        for (std::error_code ec; const auto &entry : fs::directory_iterator(dir, ec))
        {
            if (ec) return;
            if (!entry.is_regular_file(ec))
            {
                ec.clear();
                continue;
            }

            const auto number = parseNumberedFile(entry.path().filename().string(), kWalPrefix, kWalSuffix);
            if (!number)
                continue;
            if (*number < currentFileNumber)
                removeFile(entry.path(), "remove wal file failed");
        }
    }

    void cleanupOrphanedSSTables(const std::filesystem::path& dir, const std::set<uint64_t, std::greater<>> &tables)
    {
        if (!fs::exists(dir) || !fs::is_directory(dir))
            return;

        std::error_code ec;
        for (const auto &entry : fs::directory_iterator(dir, ec))
        {
            if (ec) return;
            if (!entry.is_regular_file(ec))
                continue;
            if (entry.path().extension() == ".sst")
            {
                const auto number = parseNumberedFile(entry.path().filename().string(), kSSTablePrefix, kSSTableSuffix);
                if (!number || tables.contains(*number))
                    continue;
                removeFile(entry.path(), "remove sst file failed");
            }
        }
    }
}

DB::DB(const std::filesystem::path& data_dir, const uint64_t threshold_, const uint64_t compactThreshold_) : threshold(threshold_), compactThreshold(compactThreshold_)
{
    namespace fs = std::filesystem;
    if (!fs::exists(data_dir))
        fs::create_directories(data_dir);
    else if (!fs::is_directory(data_dir))
        throw std::invalid_argument("Data directory is not a directory!");

    manifest = std::make_unique<Manifest>(data_dir / "MANIFEST");

    const auto sstableDir = data_dir / "sstable";
    const auto walDir = data_dir / "wal";
    SSTable::cleanupOrphanedTemps(sstableDir);
    cleanupOrphanedSSTables(sstableDir, manifest->allTableNumbers());

    this->data_dir = data_dir;
    walFilePath = walPath(data_dir, manifest->logNumber());
    cleanupOrphanedWAL(walDir, manifest->logNumber());
    actMemTable = std::make_unique<MemTable>(walFilePath.string());
}

bool DB::put(const std::string& key, const std::string& value)
{
    if (!actMemTable->put(key, value))
        return false;

    try
    {
        if (actMemTable->size_bytes() > threshold)
            flush();
    } catch (const std::exception &e)
    {
        std::cerr << e.what() << std::endl;
    }
    return true;
}

bool DB::get(std::string_view key, std::string& value) const
{
    const auto ret = actMemTable->get(key, value);
    if (ret == Result::ABSENT)
        return searchFromSSTable(key, value);
    return ret == Result::VALUE;
}

bool DB::remove(const std::string &key)
{
    return actMemTable->remove(key);
}

void DB::flush()
{
    if (actMemTable->size() == 0)
        return;

    namespace fs = std::filesystem;
    const auto fileNumber = manifest->allocateNumber();
    const auto ssTablePath = sstablePath(data_dir, fileNumber);
    if (!fs::exists(ssTablePath.parent_path()))
        fs::create_directories(ssTablePath.parent_path());

    const auto walFileNumber = manifest->allocateNumber();
    manifest->setLogNumber(walFileNumber);

    const auto [minKey, maxKey] = SSTable::build(*actMemTable, ssTablePath);
    manifest->addTable(fileNumber, minKey, maxKey);
    manifest->save();

    // old wal file number == current sstable file number
    const auto oldWalFilePath = walFilePath;
    const auto newWalFilePath = walPath(data_dir, walFileNumber);
    actMemTable = std::make_unique<MemTable>(newWalFilePath.string());
    walFilePath = newWalFilePath;

    removeFile(oldWalFilePath, "remove wal file failed");

    if (manifest->allTableNumbers().size() > compactThreshold)
        compact();
}

void DB::compact()
{
    namespace fs = std::filesystem;
    const fs::path inputPath = data_dir / "sstable";
    const auto outFileNumber = manifest->allocateNumber();
    auto tables = manifest->allTableNumbers();
    const std::vector removed(tables.begin(), tables.end());
    const fs::path outPath = inputPath / std::format("sst_{}.sst", outFileNumber);

    std::vector<fs::path> inputFiles;
    inputFiles.reserve(tables.size());
    for (const auto index : tables)
        inputFiles.emplace_back(inputPath / std::format("sst_{}.sst", index));

    const auto [minKey, maxKey] = SSTable::merge(inputFiles, outPath);
    manifest->replaceTables(removed, outFileNumber, minKey, maxKey);
    manifest->save();

    for (const auto &oldFilePath : inputFiles)
    {
        removeFile(oldFilePath, "remove old sst file failed");
    }
}

std::vector<Record> DB::scan(std::string_view start, std::string_view end) const
{
    std::vector<std::unique_ptr<Iterator>> iters;
    iters.emplace_back(std::make_unique<MemTableIterator>(*actMemTable));

    namespace fs = std::filesystem;
    std::vector<fs::path> inputFiles;
    for (const auto index : manifest->allTableNumbers())
        inputFiles.emplace_back(sstablePath(data_dir, index));

    std::ranges::transform(inputFiles, std::back_inserter(iters), [](const fs::path &p)
    {
        return std::make_unique<SSTableIterator>(p.string());
    });

    auto finalResult = mergeSorted(std::move(iters)) | std::ranges::views::filter([&start, &end](const Record &record)
    {
        return record.key >= start && record.key < end && record.type == Type::VALUE;
    });
    return std::ranges::to<std::vector>(finalResult);
}

bool DB::searchFromSSTable(const std::string_view key, std::string& value) const
{
    for (const auto index : manifest->allTableNumbers())
    {
        const auto filePath = sstablePath(data_dir, index);
        SSTable cur_table(filePath);
        const auto ret = cur_table.get(key, value);
        if (ret == Result::VALUE)
            return true;
        if (ret == Result::TOMBSTONE)
            return false;
    }
    return false;
}
