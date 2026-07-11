#include "utils.h"

#include <filesystem>
#include <fcntl.h>
#include <queue>

FileWriter::FileWriter(std::filesystem::path path) : path_(std::move(path)), dataFd(-1)
{
    if (parentDir = path_.parent_path(); parentDir.empty())
    {
        parentDir = ".";
    }
    else
    {
        std::filesystem::create_directories(parentDir);
    }

    tempPath = path_.string() + ".tmp";
    int fd = ::open(tempPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0664);
    if (fd < 0)
        throw std::runtime_error("Failed to open file for writing!");
    dataFd.setFd(fd);
}

uint64_t FileWriter::add(const Record &record)
{
    auto &key = record.key;
    auto &value = record.value;
    const uint32_t key_size = key.size();
    const uint32_t value_size = value.size();
    const auto type = static_cast<uint8_t>(record.type);

    writeAll(dataFd.get(), &type, sizeof(type));
    writeAll(dataFd.get(), &key_size, sizeof(key_size));
    writeAll(dataFd.get(), &value_size, sizeof(value_size));
    writeAll(dataFd.get(), key.data(), key_size);
    writeAll(dataFd.get(), value.data(), value_size);
    return 9 + key_size + value_size;
}

void FileWriter::finish()
{
    if (::fsync(dataFd.get()))
    {
        throw std::runtime_error("Failed to fsync table!");
    }

    dataFd.close();
    if (::rename(tempPath.c_str(), path_.c_str()))
    {
        const int err = errno;
        throw std::system_error(err, std::generic_category(), "rename failed");
    }

    const int dirFd = ::open(parentDir.c_str(), O_RDONLY);
    if (dirFd < 0)
        throw std::runtime_error("Failed to open dir for reading!");

    FdGuard dir(dirFd);
    if (::fsync(dir.get()))
        throw std::runtime_error("Failed to fsync dir!");
    dir.close();
}

int FileWriter::getFd() const
{
    return dataFd.get();
}

std::vector<Record> mergeSorted(std::vector<std::unique_ptr<Iterator>>& sources)
{
    std::vector<Record> result;
    struct MergeItem
    {
        std::string key;
        int index;

        bool operator<(const MergeItem &other) const
        {
            if (key != other.key)
                return key > other.key;
            return index > other.index;
        }
    };

    std::priority_queue<MergeItem> queues;
    for (int i = 0; i < sources.size(); ++i)
    {
        if (sources[i] && sources[i]->valid())
        {
            const auto &source = sources[i]->current();
            queues.emplace(source.key, i);
        }
    }

    while (!queues.empty())
    {
        auto item = queues.top();
        queues.pop();

        while (!queues.empty() && queues.top().key == item.key)
        {
            const auto [key, index] = queues.top();
            queues.pop();
            sources[index]->advance();
            if (sources[index]->valid())
            {
                auto &curRecord = sources[index]->current();
                queues.emplace(curRecord.key, index);
            }
        }

        const auto &curIt = sources[item.index];
        result.push_back(curIt->current());
        curIt->advance();
        if (curIt->valid())
            queues.emplace(curIt->current().key, item.index);
    }
    return result;
}