#ifndef LSMTREE_BLOOMFILTER_H
#define LSMTREE_BLOOMFILTER_H

#include <vector>
#include <cstdint>

class BloomFilter
{
public:
    explicit BloomFilter(unsigned int n, double p);
    void add(std::string_view key);
    bool mightContain(std::string_view key) const;

private:
    std::vector<uint64_t> bit;
    uint64_t m;
    uint64_t k;

    uint64_t getBitPosition(std::string_view key, uint64_t pos) const;
};


#endif //LSMTREE_BLOOMFILTER_H
