#include "WriteBatch.h"

void WriteBatch::put(const std::string& key, const std::string& value)
{
    records_.emplace_back(key, Type::VALUE, value);
}

void WriteBatch::remove(const std::string& key) { records_.emplace_back(key, Type::TOMBSTONE); }

void WriteBatch::clear() { records_.clear(); }
