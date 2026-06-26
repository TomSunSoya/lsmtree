#include "DB.h"

#include <format>
#include <iostream>

#include "SSTable.h"

namespace
{
    void cleanupOrphanedWAL(const std::filesystem::path& dir, const uint64_t currentFileNumber)
    {
        namespace fs = std::filesystem;
        if (!fs::exists(dir) || !fs::is_directory(dir))
            return;

        constexpr std::string_view prefix = "wal_", suffix = ".wal";
        for (std::error_code ec; const auto &entry : fs::directory_iterator(dir, ec))
        {
            if (ec) return;
            if (!entry.is_regular_file(ec))
            {
                ec.clear();
                continue;
            }

            const auto number = parseNumberedFile(entry.path().filename().string(), prefix, suffix);
            if (!number)
                continue;
            if (*number < currentFileNumber)
            {
                if (::remove(entry.path().c_str()))
                {
                    const auto err = errno;
                    throw std::system_error(err, std::system_category(), "remove wal file failed");
                }
            }
        }
    }
}

DB::DB(const std::filesystem::path& data_dir, const uint64_t threshold_) : threshold(threshold_)
{
    namespace fs = std::filesystem;
    if (!fs::exists(data_dir))
        fs::create_directories(data_dir);
    else if (!fs::is_directory(data_dir))
        throw std::invalid_argument("Data directory is not a directory!");

    manifest = std::make_unique<Manifest>(data_dir / "MANIFEST");

    SSTable::cleanupOrphanedTemps(data_dir / "sstable");
    const auto fileNumber = manifest->nextNumber();

    this->data_dir = data_dir;
    walFilePath = data_dir / "wal" / std::format("wal_{}.wal", fileNumber);
    cleanupOrphanedWAL(data_dir / "wal", fileNumber);
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
    namespace fs = std::filesystem;
    const auto fileNumber = manifest->allocateNumber();
    const auto ssTablePath = data_dir / "sstable" / std::format("sst_{}.sst", fileNumber);
    if (!fs::exists(ssTablePath.parent_path()))
        fs::create_directories(ssTablePath.parent_path());

    SSTable::build(*actMemTable, ssTablePath);
    manifest->addTable(fileNumber);
    manifest->save();

    // old wal file number == current sstable file number
    const auto oldWalFilePath = walFilePath;
    const auto newWalFilePath = data_dir / "wal" / std::format("wal_{}.wal", manifest->nextNumber());
    actMemTable = std::make_unique<MemTable>(newWalFilePath.string());
    walFilePath = newWalFilePath;

    if ( ::remove(oldWalFilePath.c_str()))
    {
        const int err = errno;
        throw std::system_error(err, std::system_category(), "remove wal file failed");
    }
}

bool DB::searchFromSSTable(std::string_view key, std::string& value) const
{
    for (auto index : manifest->tables())
    {
        const std::filesystem::path filePath = data_dir / "sstable" / std::format("sst_{}.sst", index);
        SSTable cur_table(filePath);
        const auto ret = cur_table.get(key, value);
        if (ret == Result::VALUE)
            return true;
        if (ret == Result::TOMBSTONE)
            return false;
    }
    return false;
}
