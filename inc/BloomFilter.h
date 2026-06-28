#ifndef LSMTREE_BLOOMFILTER_H
#define LSMTREE_BLOOMFILTER_H

#include <vector>
#include <span>

class BloomFilter
{
public:
    static std::vector<std::byte> Serialize(const BloomFilter &bloom_filter);
    static BloomFilter fromBytes(std::span<const std::byte> data);

    explicit BloomFilter(unsigned int n, double p);
    BloomFilter(uint64_t m, uint64_t k);
    void add(std::string_view key);
    [[nodiscard]] bool mightContain(std::string_view key) const;

private:
    std::vector<uint64_t> bit;
    uint64_t m;
    uint64_t k;

    [[nodiscard]] uint64_t getBitPosition(std::string_view key, uint64_t pos) const;
};


#endif //LSMTREE_BLOOMFILTER_H
