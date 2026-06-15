#include "DB.h"

DB::DB(const std::filesystem::path& data_dir)
{
    namespace fs = std::filesystem;
    if (!fs::exists(data_dir))
        fs::create_directories(data_dir);
    else if (!fs::is_directory(data_dir))
        throw std::invalid_argument("Data directory is not a directory!");

    this->data_dir = data_dir;
    walFilePath = data_dir / "wal" / "wal1.wal";
    actMemTable = std::make_unique<MemTable>(walFilePath.string());
}

bool DB::put(const std::string& key, const std::string& value)
{
    return actMemTable->put(key, value);
}

bool DB::get(std::string_view key, std::string& value) const
{
    return actMemTable->get(key, value);
}
