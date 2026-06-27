#ifndef LSMTREE_MANIFEST_H
#define LSMTREE_MANIFEST_H

#include <filesystem>
#include <cstdint>
#include <functional>
#include <set>
#include <vector>

class Manifest
{
public:
    explicit Manifest(std::filesystem::path  path);

    // search
    [[nodiscard]] const std::set<uint64_t, std::greater<>> &tables() const;
    [[nodiscard]] uint64_t nextNumber() const;

    // memory modify
    uint64_t allocateNumber();
    void addTable(uint64_t n);
    void replaceTables(const std::vector<uint64_t> &removed, uint64_t added);

    void save() const;

    [[nodiscard]] uint64_t logNumber() const
    {
        return logNumber_;
    }

    void setLogNumber(const uint64_t log_number)
    {
        logNumber_ = log_number;
    }

private:
    std::filesystem::path path_;
    std::set<uint64_t, std::greater<>> tables_;
    uint64_t next_{};
    uint8_t version_{};
    uint64_t logNumber_{};
};


#endif //LSMTREE_MANIFEST_H
