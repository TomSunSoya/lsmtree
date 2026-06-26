

#include "Manifest.h"

#include <format>
#include <utility>
#include <fstream>

#include "utils.h"

Manifest::Manifest(std::filesystem::path path) : path_(std::move(path))
{
    if (std::ifstream ifs(path_); ifs)
    {
        std::string line;
        while (std::getline(ifs, line))
        {
            auto pos = line.find(':');
            if (pos == std::string::npos)
                throw std::runtime_error("Failed to find ':'");
            std::string name = line.substr(0, pos);
            std::string numStr = line.substr(pos + 1);
            uint64_t num = std::stoull(numStr);
            if (name == "version")
                version_ = num;
            else if (name == "next")
                next_ = num;
            else
                tables_.insert(num);
        }
    }
}

const std::set<uint64_t, std::greater<>>& Manifest::tables() const
{
    return tables_;
}

uint64_t Manifest::nextNumber() const
{
    return next_;
}

uint64_t Manifest::allocateNumber()
{
    return next_++;
}

void Manifest::addTable(uint64_t n)
{
    tables_.insert(n);
}

void Manifest::replaceTables(const std::vector<uint64_t>& removed, uint64_t added)
{
    for (auto remove : removed)
        tables_.erase(remove);
    tables_.insert(added);
}

void Manifest::save() const
{
    FileWriter writer(path_);
    std::string versionStr = std::format("version:{}\n", version_);
    std::string nextStr = std::format("next:{}\n", next_);
    writeAll(writer.getFd(), versionStr.data(), versionStr.size());
    writeAll(writer.getFd(), nextStr.data(), nextStr.size());
    for (auto table : tables_)
    {
        std::string line = std::format("table:{}\n", table);
        writeAll(writer.getFd(), line.data(), line.length());
    }
    writer.finish();
}
