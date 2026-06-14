#include <utility>

#include "SSTable.h"
#include <sstream>

void SSTable::build(const MemTable& mt, const std::filesystem::path& path)
{
    if (const auto dir = path.parent_path(); !dir.empty())
        std::filesystem::create_directories(dir);

    if (std::filesystem::exists(path))
        throw std::runtime_error("SSTable has been exist!");

    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    for (auto &[key, value] : mt)
    {
        constexpr uint8_t type = 0;
        const uint32_t key_size = key.size();
        const uint32_t value_size = value.size();
        out.write(reinterpret_cast<const char*>(&type), 1);
        out.write(reinterpret_cast<const char*>(&key_size), 4);
        out.write(reinterpret_cast<const char*>(&value_size), 4);
        out.write(key.data(), key_size);
        out.write(value.data(), value_size);
    }
}

SSTable::SSTable(std::filesystem::path path) : path(std::move(path))
{
}

bool SSTable::get(std::string_view key, std::string& value) const
{
    if (!std::filesystem::exists(path))
        return false;

    std::ifstream ifs{path, std::ios::binary};
    if (!ifs.is_open())
        return false;

    char type = 0;
    while (ifs.read(&type, sizeof(char)))
    {
        uint32_t key_size{}, value_size{};
        if (!ifs.read(reinterpret_cast<char *>(&key_size),  sizeof(key_size)))
            return false;
        if (!ifs.read(reinterpret_cast<char *>(&value_size), sizeof(value_size)))
            return false;

        std::string cur_key(key_size, 0);
        std::string cur_value(value_size, 0);

        if (!ifs.read(cur_key.data(), key_size)) return false;
        if (!ifs.read(cur_value.data(), value_size)) return false;
        if (cur_key == key)
        {
            value = cur_value;
            return true;
        }
    }
    return false;
}