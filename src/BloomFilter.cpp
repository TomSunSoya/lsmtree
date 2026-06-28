#include "BloomFilter.h"

#include <cmath>
BloomFilter::BloomFilter(const unsigned int n, const double p)
{
    static const double ln2 = std::log(2.0);
    const double mDouble = -static_cast<double>(n) * std::log(p) / (ln2 * ln2);
    m = static_cast<uint64_t>(std::ceil(mDouble));
    k = static_cast<uint64_t>(std::round(mDouble / n * ln2));
    bit.resize((m + 63) / 64);
}

void BloomFilter::add(const std::string_view key)
{
    for (uint64_t i = 0; i < k; ++i)
    {
        const uint64_t pos = getBitPosition(key, i);
        bit[pos / 64] |= 1ull << (pos % 64);
    }
}

bool BloomFilter::mightContain(const std::string_view key) const
{
    for (uint64_t i = 0; i < k; ++i)
    {
        if (const uint64_t pos = getBitPosition(key, i); (bit[pos / 64] & 1ull << (pos % 64)) == 0)
            return false;
    }
    return true;
}

uint64_t BloomFilter::getBitPosition(const std::string_view key, const uint64_t pos) const
{
    const size_t h = std::hash<std::string_view>{}(key);
    const uint32_t h1 = h >> 32;
    uint32_t h2 = h & 0xFFFFFFFF;
    if (h2 == 0)
    {
        h2 = 1;
    }
    return (h1 + pos * h2) % m;
}
