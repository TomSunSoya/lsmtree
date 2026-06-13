#ifndef LSMTREE_MEMTABLE_H
#define LSMTREE_MEMTABLE_H

#include <map>
#include <string>
#include <string_view>
#include <filesystem>
#include <vector>
#include <fstream>
#include <unistd.h>

class MemTable
{
public:
    explicit MemTable(std::string_view logFilePath);

    bool put(const std::string &key, const std::string &value);
    bool get(std::string_view key, std::string &value) const;

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

    std::map<std::string, std::string, std::less<>> table;
    const std::filesystem::path log_path;
    FileWriter writer;

    void putToWAL(const std::string &key, const std::string &value);
    bool restoreFromWAL();
    static std::vector<std::pair<std::string, std::string>> parseWALRecord(std::string_view content, size_t &lastGoodOffset);
};


#endif //LSMTREE_MEMTABLE_H
