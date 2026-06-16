#ifndef LSMTREE_DB_H
#define LSMTREE_DB_H

#include <memory>
#include "MemTable.h"

class DB
{
public:
    explicit DB(const std::filesystem::path &data_dir, uint64_t threshold = 5 * 1024 * 1024);

    bool put(const std::string &key, const std::string &value);
    bool get(std::string_view key, std::string &value) const;
    void flush();

private:
    std::unique_ptr<MemTable> actMemTable;
    std::filesystem::path data_dir;
    std::filesystem::path walFilePath;
    uint64_t currentFileNumber = 0;
    uint64_t threshold;

    bool searchFromSSTable(std::string_view key, std::string& value) const;
};


#endif //LSMTREE_DB_H
