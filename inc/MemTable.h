#ifndef LSMTREE_MEMTABLE_H
#define LSMTREE_MEMTABLE_H

#include <map>
#include <string>
#include <string_view>
#include <filesystem>
#include <vector>
#include <fstream>
#include <unistd.h>

#include "utils.h"

class MemTable
{
public:
    explicit MemTable(std::string_view logFilePath);

    bool put(const std::string &key, const std::string &value);
    Result get(std::string_view key, std::string &value) const;
    bool remove(const std::string &key);

    using const_iterator = std::map<std::string, Entry, std::less<>>::const_iterator;
    [[nodiscard]] const_iterator begin() const noexcept { return table.begin(); }
    [[nodiscard]] const_iterator end() const noexcept { return table.end(); }

    [[nodiscard]] size_t size_bytes() const;

private:
    struct FileWriter
    {
        std::filesystem::path path;
        int fd = -1;
        bool poisoned = false;

        explicit FileWriter(std::string_view logPath);
        FileWriter(const FileWriter &other) = delete;
        ~FileWriter() {::close(fd);};

        FileWriter &operator=(const FileWriter &other) = delete;

        void write(const std::string &record);
        int truncate(size_t offset);
    };

    std::map<std::string, Entry, std::less<>> table;
    const std::filesystem::path log_path;
    FileWriter writer;
    size_t current_size;

    void putToWAL(const std::string &key, const std::string &value, const Type type);
    bool restoreFromWAL();
    static std::vector<std::pair<std::string, Entry>> parseWALRecord(std::string_view content, size_t &lastGoodOffset);
};


#endif //LSMTREE_MEMTABLE_H
