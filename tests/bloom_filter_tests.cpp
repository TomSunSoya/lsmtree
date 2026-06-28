#include "BloomFilter.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace
{
std::string indexedKey(const std::string_view prefix, const size_t index)
{
    return std::string(prefix) + std::to_string(index);
}
}

TEST(BloomFilterTest, EmptyFilterRejectsKeys)
{
    const BloomFilter filter(100, 0.01);

    EXPECT_FALSE(filter.mightContain(""));
    EXPECT_FALSE(filter.mightContain("alpha"));
    EXPECT_FALSE(filter.mightContain("not-added"));
}

TEST(BloomFilterTest, AddedKeysNeverBecomeFalseNegatives)
{
    BloomFilter filter(10, 0.01);
    const std::vector<std::string> keys{
        "",
        "alpha",
        "alpha=beta,with-delimiters",
        std::string("embedded\0null", 13),
    };

    for (const auto &key : keys)
        filter.add(key);

    for (const auto &key : keys)
        EXPECT_TRUE(filter.mightContain(key)) << "false negative for key of size " << key.size();
}

TEST(BloomFilterTest, FalsePositiveRateStaysNearConfiguredProbability)
{
    constexpr size_t expectedEntries = 10'000;
    constexpr size_t queryCount = 20'000;
    constexpr double configuredProbability = 0.01;
    constexpr double maximumAcceptedRate = 0.03;
    BloomFilter filter(expectedEntries, configuredProbability);

    for (size_t i = 0; i < expectedEntries; ++i)
        filter.add(indexedKey("inserted-", i));

    for (size_t i = 0; i < expectedEntries; ++i)
        ASSERT_TRUE(filter.mightContain(indexedKey("inserted-", i))) << "false negative at index " << i;

    size_t falsePositives = 0;
    for (size_t i = 0; i < queryCount; ++i)
    {
        if (filter.mightContain(indexedKey("absent-", i)))
            ++falsePositives;
    }

    const double observedRate = static_cast<double>(falsePositives) / queryCount;
    EXPECT_LT(observedRate, maximumAcceptedRate)
        << "observed " << falsePositives << " false positives out of " << queryCount;
}
