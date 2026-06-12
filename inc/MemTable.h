#ifndef LSMTREE_MEMTABLE_H
#define LSMTREE_MEMTABLE_H

#include <map>
#include <string>
#include <string_view>
#include <filesystem>
#include <vector>
#include <fstream>

class MemTable
{
public:
    MemTable(std::string_view logFilePath);

    bool put(const std::string &key, const std::string &value);
    bool get(std::string_view key, std::string &value) const;

private:
    std::map<std::string, std::string, std::less<>> table;
    const std::filesystem::path log_path;
    std::ofstream out;

    void putToWAL(const std::string &key, const std::string &value);

    static std::vector<std::pair<std::string, std::string>> parseWALRecord(std::string_view content);
};


#endif //LSMTREE_MEMTABLE_H
