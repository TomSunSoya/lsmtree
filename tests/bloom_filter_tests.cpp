#include "BloomFilter.h"

#include <cstddef>
#include <cstring>
#include <stdexcept>
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
} // namespace

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

    for (const auto& key : keys)
        filter.add(key);

    for (const auto& key : keys)
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

TEST(BloomFilterTest, SerializationRoundTripPreservesMembership)
{
    constexpr size_t expectedEntries = 1'000;
    BloomFilter original(expectedEntries, 0.01);
    std::vector<std::string> insertedKeys;
    insertedKeys.reserve(expectedEntries);

    for (size_t i = 0; i < expectedEntries; ++i)
    {
        insertedKeys.push_back(indexedKey("round-trip-", i));
        original.add(insertedKeys.back());
    }

    const auto bytes = BloomFilter::Serialize(original);
    const BloomFilter restored = BloomFilter::fromBytes(bytes);

    EXPECT_GT(bytes.size(), 2 * sizeof(uint64_t));
    EXPECT_EQ(0, bytes.size() % sizeof(uint64_t));
    EXPECT_EQ(bytes, BloomFilter::Serialize(restored));

    for (const auto& key : insertedKeys)
        ASSERT_TRUE(restored.mightContain(key)) << "false negative after restoring key: " << key;

    for (size_t i = 0; i < expectedEntries; ++i)
    {
        const auto key = indexedKey("never-inserted-", i);
        EXPECT_EQ(original.mightContain(key), restored.mightContain(key))
            << "membership decision changed after restoring key: " << key;
    }
}

TEST(BloomFilterTest, PersistedHashMatchesDocumentedFNV1aVector)
{
    // Persisted Bloom bits are part of the SSTable format. Pin the base hash to
    // 64-bit FNV-1a so a different standard library, compiler, or process cannot
    // reinterpret an existing table and introduce a false negative.
    BloomFilter filter(uint64_t{64}, uint64_t{1});
    filter.add("alpha");

    const auto bytes = BloomFilter::Serialize(filter);
    ASSERT_EQ(3 * sizeof(uint64_t), bytes.size());

    uint64_t word = 0;
    std::memcpy(&word, bytes.data() + 2 * sizeof(uint64_t), sizeof(word));

    // FNV-1a("alpha") == 0x8ac625bb85ed202b. The current double-hash
    // scheme uses the upper 32 bits for hash index zero: 0x8ac625bb % 64 == 59.
    EXPECT_EQ(uint64_t{1} << 59, word);
}

TEST(BloomFilterTest, FromBytesRejectsInvalidEnvelopeSizes)
{
    const std::vector<std::byte> shorterThanHeader(2 * sizeof(uint64_t) - 1);
    const std::vector<std::byte> misaligned(2 * sizeof(uint64_t) + 1);

    EXPECT_THROW(BloomFilter::fromBytes(shorterThanHeader), std::length_error);
    EXPECT_THROW(BloomFilter::fromBytes(misaligned), std::length_error);
}
