#include "DB.h"

#include <format>

namespace
{
    uint64_t maxFileByName(const std::filesystem::path &path)
    {
        namespace fs = std::filesystem;
        if (!fs::exists(path) || !fs::is_directory(path))
            return 0;

        const auto parseSSTableNumber = [] (const std::string &filename) -> std::optional<uint64_t>
        {
            constexpr std::string_view prefix = "sst_";
            constexpr std::string_view suffix = ".sst";

            if (filename.size() <= prefix.size() + suffix.size())
                return std::nullopt;
            if (!filename.starts_with(prefix))
                return std::nullopt;
            if (!filename.ends_with(suffix))
                return std::nullopt;

            std::string numberStr{filename.substr(prefix.size(), filename.size() - prefix.size() - suffix.size())};

            try
            {
                size_t pos = 0;
                auto value = std::stoull(numberStr, &pos, 10);

                if (pos != numberStr.size())
                    return std::nullopt;

                return static_cast<uint64_t>(value);
            } catch (const std::exception &)
            {
                return std::nullopt;
            }
        };

        std::error_code ec;
        std::optional<uint64_t> currentFileNumber = std::nullopt;
        for (const auto &entry : fs::directory_iterator(path, ec))
        {
            if (ec) return 0;
            if (!entry.is_regular_file(ec))
            {
                ec.clear();
                continue;
            }

            auto number = parseSSTableNumber(entry.path().filename());
            if (!number)
                continue;
            if (!currentFileNumber || *currentFileNumber < *number)
                currentFileNumber = *number;
        }
        if (currentFileNumber)
            ++*currentFileNumber;
        return currentFileNumber ? *currentFileNumber : 0;
    }
}

DB::DB(const std::filesystem::path& data_dir)
{
    namespace fs = std::filesystem;
    if (!fs::exists(data_dir))
        fs::create_directories(data_dir);
    else if (!fs::is_directory(data_dir))
        throw std::invalid_argument("Data directory is not a directory!");

    currentFileNumber = maxFileByName(data_dir / "sstable");

    this->data_dir = data_dir;
    walFilePath = data_dir / "wal" / std::format("wal_{}.wal", currentFileNumber);
    actMemTable = std::make_unique<MemTable>(walFilePath.string());
}

bool DB::put(const std::string& key, const std::string& value)
{
    return actMemTable->put(key, value);
}

bool DB::get(std::string_view key, std::string& value) const
{
    if (!actMemTable->get(key, value))
        return searchFromSSTable(key, value);
    return true;
}

void DB::flush()
{
    namespace fs = std::filesystem;
    const auto ssTablePath = data_dir / "sstable" / std::format("sst_{}.sst", currentFileNumber++);
    if (!fs::exists(ssTablePath.parent_path()))
        fs::create_directories(ssTablePath.parent_path());

    SSTable::build(*actMemTable, ssTablePath);

    actMemTable.reset();

    if (::remove(walFilePath.c_str()))
    {
        const int err = errno;
        throw std::system_error(err, std::system_category(), "remove wal file failed");
    }

    walFilePath = data_dir / "wal" / std::format("wal_{}.wal", currentFileNumber);
    actMemTable = std::make_unique<MemTable>(walFilePath.string());
}

bool DB::searchFromSSTable(std::string_view key, std::string& value) const
{
    if (!currentFileNumber) return false;
    for (int64_t i = currentFileNumber - 1; i >= 0; --i)
    {
        const std::filesystem::path filePath = data_dir / "sstable" / std::format("sst_{}.sst", i);
        SSTable cur_table(filePath);
        if (cur_table.get(key, value))
            return true;
    }
    return false;
}
