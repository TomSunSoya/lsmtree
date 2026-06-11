#include "MemTable.h"

void MemTable::put(const std::string& key, const std::string& value)
{
    table[key] = value;
}

bool MemTable::get(const std::string_view key, std::string &value) const
{
    const auto it = table.find(key);
    if (it == table.end())
        return false;
    value = it->second;
    return true;
}