#ifndef LSMTREE_MEMTABLE_H
#define LSMTREE_MEMTABLE_H

#include <map>
#include <string>
#include <string_view>

class MemTable
{
public:
    void put(const std::string &key, const std::string &value);
    bool get(std::string_view key, std::string &value) const;

private:
    std::map<std::string, std::string, std::less<>> table;
};


#endif //LSMTREE_MEMTABLE_H
