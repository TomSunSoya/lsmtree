#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "MemTable.h"
#include "SSTable.h"
#include "test_support.h"

namespace
{
using test_support::readFile;
using test_support::ScopedPathCleanup;
using test_support::writeFile;

std::string multiBlockKey(const size_t index)
{
    return "key-" + std::string(index < 10 ? "0" : "") + std::to_string(index);
}

void expectPut(MemTable& table, const std::string& key, const std::string& value)
{
    ASSERT_TRUE(table.put(key, value)) << "expected put to succeed for key: " << key;
}

void expectGet(const SSTable& table, const std::string& key, const std::string& expected)
{
    std::string actual;
    ASSERT_EQ(Result::VALUE, table.get(key, actual)) << "expected key to exist: " << key;
    EXPECT_EQ(expected, actual) << "unexpected value for key: " << key;
}

void expectMissing(const SSTable& table, const std::string& key)
{
    std::string actual = "unchanged";
    EXPECT_EQ(Result::ABSENT, table.get(key, actual)) << "expected key to be missing: " << key;
}

void expectTombstone(const SSTable& table, const std::string& key)
{
    std::string actual = "unchanged";
    EXPECT_EQ(Result::TOMBSTONE, table.get(key, actual)) << "expected tombstone for key: " << key;
}

std::string hexDump(std::string_view content)
{
    std::ostringstream out;
    out << std::hex << std::setfill('0');

    for (size_t i = 0; i < content.size(); ++i)
    {
        if (i != 0)
            out << ' ';
        out << std::setw(2) << static_cast<unsigned int>(static_cast<unsigned char>(content[i]));
    }
    return out.str();
}

void expectRecord(const Record& record, const std::string& key, const Type type, const std::string& value)
{
    EXPECT_EQ(key, record.key);
    EXPECT_EQ(type, record.type);
    EXPECT_EQ(value, record.value);
}

} // namespace

TEST(SSTableTest, BuildAndReadRecordsFromMemTable)
{
    const std::filesystem::path walPath("sstable_tests_build_read.wal");
    const std::filesystem::path sstablePath("sstable_tests_build_read.sst");
    const ScopedPathCleanup cleanup({walPath, sstablePath});

    {
        MemTable memTable(walPath.string());
        ASSERT_NO_FATAL_FAILURE(expectPut(memTable, "beta", "two"));
        ASSERT_NO_FATAL_FAILURE(expectPut(memTable, "alpha", "one"));

        ASSERT_NO_THROW(SSTable::build(memTable, sstablePath));
    }

    ASSERT_TRUE(std::filesystem::exists(sstablePath));

    const SSTable sstable(sstablePath);
    ASSERT_NO_FATAL_FAILURE(expectGet(sstable, "alpha", "one"));
    ASSERT_NO_FATAL_FAILURE(expectGet(sstable, "beta", "two"));
    expectMissing(sstable, "gamma");
}

TEST(SSTableTest, BuildPersistsTombstoneAndKeepsScanningUnrelatedKeys)
{
    const std::filesystem::path walPath("sstable_tests_tombstone.wal");
    const std::filesystem::path sstablePath("sstable_tests_tombstone.sst");
    const ScopedPathCleanup cleanup({walPath, sstablePath});

    {
        MemTable memTable(walPath.string());
        ASSERT_NO_FATAL_FAILURE(expectPut(memTable, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectPut(memTable, "beta", "two"));
        ASSERT_TRUE(memTable.remove("alpha"));

        ASSERT_NO_THROW(SSTable::build(memTable, sstablePath));
    }

    const SSTable sstable(sstablePath);
    expectTombstone(sstable, "alpha");
    ASSERT_NO_FATAL_FAILURE(expectGet(sstable, "beta", "two"));
    expectMissing(sstable, "gamma");
}

TEST(SSTableTest, BuildReturnsMinAndMaxKeysIncludingTombstones)
{
    const std::filesystem::path root("sstable_tests_build_returns_range");
    const ScopedPathCleanup cleanup(root);
    ASSERT_TRUE(std::filesystem::create_directories(root));
    const std::filesystem::path sstablePath = root / "sst_0.sst";

    std::pair<std::string, std::string> keyRange;
    {
        MemTable memTable((root / "wal_0.wal").string());
        ASSERT_NO_FATAL_FAILURE(expectPut(memTable, "middle", "value"));
        ASSERT_NO_FATAL_FAILURE(expectPut(memTable, "zulu", "last"));
        ASSERT_TRUE(memTable.remove("alpha"));

        ASSERT_NO_THROW(keyRange = SSTable::build(memTable, sstablePath));
    }

    EXPECT_EQ("alpha", keyRange.first);
    EXPECT_EQ("zulu", keyRange.second);

    const SSTable sstable(sstablePath);
    ASSERT_NO_FATAL_FAILURE(expectTombstone(sstable, "alpha"));
    ASSERT_NO_FATAL_FAILURE(expectGet(sstable, "middle", "value"));
    ASSERT_NO_FATAL_FAILURE(expectGet(sstable, "zulu", "last"));
}

TEST(SSTableTest, IteratorTraversesRecordsInTableOrderThroughBaseInterface)
{
    const std::filesystem::path walPath("sstable_tests_cursor_iterate.wal");
    const std::filesystem::path sstablePath("sstable_tests_cursor_iterate.sst");
    const ScopedPathCleanup cleanup({walPath, sstablePath});

    {
        MemTable memTable(walPath.string());
        ASSERT_NO_FATAL_FAILURE(expectPut(memTable, "beta", "two"));
        ASSERT_NO_FATAL_FAILURE(expectPut(memTable, "alpha", "one"));

        ASSERT_NO_THROW(SSTable::build(memTable, sstablePath));
    }

    SSTableIterator concreteIterator(sstablePath);
    Iterator& iterator = concreteIterator;
    ASSERT_TRUE(iterator.valid());
    ASSERT_NO_FATAL_FAILURE(expectRecord(iterator.current(), "alpha", Type::VALUE, "one"));

    iterator.advance();
    ASSERT_TRUE(iterator.valid());
    ASSERT_NO_FATAL_FAILURE(expectRecord(iterator.current(), "beta", Type::VALUE, "two"));

    iterator.advance();
    EXPECT_FALSE(iterator.valid());
}

TEST(SSTableTest, IteratorExposesTombstoneRecords)
{
    const std::filesystem::path walPath("sstable_tests_cursor_tombstone.wal");
    const std::filesystem::path sstablePath("sstable_tests_cursor_tombstone.sst");
    const ScopedPathCleanup cleanup({walPath, sstablePath});

    {
        MemTable memTable(walPath.string());
        ASSERT_NO_FATAL_FAILURE(expectPut(memTable, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectPut(memTable, "beta", "two"));
        ASSERT_TRUE(memTable.remove("alpha"));

        ASSERT_NO_THROW(SSTable::build(memTable, sstablePath));
    }

    SSTableIterator cursor(sstablePath);
    ASSERT_TRUE(cursor.valid());
    ASSERT_NO_FATAL_FAILURE(expectRecord(cursor.current(), "alpha", Type::TOMBSTONE, ""));

    cursor.advance();
    ASSERT_TRUE(cursor.valid());
    ASSERT_NO_FATAL_FAILURE(expectRecord(cursor.current(), "beta", Type::VALUE, "two"));

    cursor.advance();
    EXPECT_FALSE(cursor.valid());
}

TEST(SSTableTest, IteratorRejectsMissingFile)
{
    const std::filesystem::path sstablePath("sstable_tests_cursor_missing.sst");
    const ScopedPathCleanup cleanup(sstablePath);

    EXPECT_THROW(SSTableIterator cursor(sstablePath), std::runtime_error);
}

TEST(SSTableTest, ParseNumberedFileAcceptsOnlyExactNumericMiddle)
{
    const auto zero = parseNumberedFile("sst_0.sst", "sst_", ".sst");
    ASSERT_TRUE(zero);
    EXPECT_EQ(0, *zero);

    const auto padded = parseNumberedFile("wal_007.wal", "wal_", ".wal");
    ASSERT_TRUE(padded);
    EXPECT_EQ(7, *padded);

    EXPECT_FALSE(parseNumberedFile("sst_.sst", "sst_", ".sst"));
    EXPECT_FALSE(parseNumberedFile("sst_12x.sst", "sst_", ".sst"));
    EXPECT_FALSE(parseNumberedFile("tmp_sst_12.sst", "sst_", ".sst"));
    EXPECT_FALSE(parseNumberedFile("sst_12.sst.tmp", "sst_", ".sst"));
}

TEST(SSTableTest, BuildWritesExpectedLittleEndianRecordPrefix)
{
    const std::filesystem::path walPath("sstable_tests_exact_bytes.wal");
    const std::filesystem::path sstablePath("sstable_tests_exact_bytes.sst");
    const ScopedPathCleanup cleanup({walPath, sstablePath});
    constexpr size_t expectedRecordsSize = 25;

    {
        MemTable memTable(walPath.string());
        ASSERT_NO_FATAL_FAILURE(expectPut(memTable, "long", ""));
        ASSERT_NO_FATAL_FAILURE(expectPut(memTable, "a", "xy"));

        ASSERT_NO_THROW(SSTable::build(memTable, sstablePath));
    }

    std::string content;
    ASSERT_NO_FATAL_FAILURE(readFile(sstablePath, content));

    ASSERT_GE(content.size(), expectedRecordsSize);
    EXPECT_EQ("00 01 00 00 00 02 00 00 00 61 78 79 "
              "00 04 00 00 00 00 00 00 00 6c 6f 6e 67",
              hexDump(std::string_view(content).substr(0, expectedRecordsSize)));
}

TEST(SSTableTest, BuildCreatesParentDirectories)
{
    const std::filesystem::path walPath("sstable_tests_nested.wal");
    const std::filesystem::path root("sstable_tests_nested");
    const std::filesystem::path sstablePath = root / "child" / "table.sst";
    const ScopedPathCleanup cleanup({walPath, root});

    {
        MemTable memTable(walPath.string());
        ASSERT_NO_FATAL_FAILURE(expectPut(memTable, "parent", "created"));

        ASSERT_NO_THROW(SSTable::build(memTable, sstablePath));
    }

    ASSERT_TRUE(std::filesystem::exists(sstablePath)) << "expected SSTable file in nested directory";

    const SSTable sstable(sstablePath);
    ASSERT_NO_FATAL_FAILURE(expectGet(sstable, "parent", "created"));
}

TEST(SSTableTest, BuildDoesNotLeaveTemporaryFileAfterSuccess)
{
    const std::filesystem::path walPath("sstable_tests_no_tmp_after_success.wal");
    const std::filesystem::path sstablePath("sstable_tests_no_tmp_after_success.sst");
    const std::filesystem::path tempPath(sstablePath.string() + ".tmp");
    const ScopedPathCleanup cleanup({walPath, sstablePath, tempPath});

    {
        MemTable memTable(walPath.string());
        ASSERT_NO_FATAL_FAILURE(expectPut(memTable, "stable", "table"));

        ASSERT_NO_THROW(SSTable::build(memTable, sstablePath));
    }

    ASSERT_TRUE(std::filesystem::exists(sstablePath));
    EXPECT_FALSE(std::filesystem::exists(tempPath))
        << "successful build should publish by rename and leave no .tmp file";

    const SSTable sstable(sstablePath);
    ASSERT_NO_FATAL_FAILURE(expectGet(sstable, "stable", "table"));
}

TEST(SSTableTest, BuildOverwritesStaleTemporaryFile)
{
    const std::filesystem::path walPath("sstable_tests_stale_tmp.wal");
    const std::filesystem::path sstablePath("sstable_tests_stale_tmp.sst");
    const std::filesystem::path tempPath(sstablePath.string() + ".tmp");
    const ScopedPathCleanup cleanup({walPath, sstablePath, tempPath});
    ASSERT_NO_FATAL_FAILURE(writeFile(tempPath, "stale temporary bytes"));

    {
        MemTable memTable(walPath.string());
        ASSERT_NO_FATAL_FAILURE(expectPut(memTable, "fresh", "value"));

        ASSERT_NO_THROW(SSTable::build(memTable, sstablePath));
    }

    ASSERT_TRUE(std::filesystem::exists(sstablePath));
    EXPECT_FALSE(std::filesystem::exists(tempPath)) << "stale .tmp file should be consumed by the successful build";

    const SSTable sstable(sstablePath);
    ASSERT_NO_FATAL_FAILURE(expectGet(sstable, "fresh", "value"));
    expectMissing(sstable, "stale");
}

TEST(SSTableTest, BuildRejectsExistingFileWithoutOverwriting)
{
    const std::filesystem::path originalWalPath("sstable_tests_existing_original.wal");
    const std::filesystem::path replacementWalPath("sstable_tests_existing_replacement.wal");
    const std::filesystem::path sstablePath("sstable_tests_existing.sst");
    const std::filesystem::path tempPath(sstablePath.string() + ".tmp");
    const ScopedPathCleanup cleanup({originalWalPath, replacementWalPath, sstablePath, tempPath});

    {
        MemTable memTable(originalWalPath.string());
        ASSERT_NO_FATAL_FAILURE(expectPut(memTable, "original", "value"));

        ASSERT_NO_THROW(SSTable::build(memTable, sstablePath));
    }

    std::string originalContent;
    ASSERT_NO_FATAL_FAILURE(readFile(sstablePath, originalContent));

    {
        MemTable memTable(replacementWalPath.string());
        ASSERT_NO_FATAL_FAILURE(expectPut(memTable, "replacement", "data"));

        EXPECT_THROW(SSTable::build(memTable, sstablePath), std::runtime_error);
    }

    EXPECT_FALSE(std::filesystem::exists(tempPath))
        << "rejecting an existing target should not publish or leave a .tmp file";
    std::string contentAfterRejectedBuild;
    ASSERT_NO_FATAL_FAILURE(readFile(sstablePath, contentAfterRejectedBuild));
    EXPECT_EQ(originalContent, contentAfterRejectedBuild);

    const SSTable sstable(sstablePath);
    ASSERT_NO_FATAL_FAILURE(expectGet(sstable, "original", "value"));
}

TEST(SSTableTest, AddRecordToFileWritesOnlyRequestedSpan)
{
    const std::filesystem::path root("sstable_tests_add_record_span");
    const ScopedPathCleanup cleanup(root);
    std::filesystem::create_directories(root);
    const std::filesystem::path sstablePath = root / "sst_0.sst";
    std::vector<Record> records{
        {"alpha", Type::VALUE, "one"},
        {"beta", Type::TOMBSTONE, ""},
        {"gamma", Type::VALUE, "three"},
        {"zulu", Type::VALUE, "last"},
    };

    ASSERT_NO_THROW(SSTable::addRecordToFile(std::span(records).subspan(1, 2), sstablePath));

    const SSTable table(sstablePath);
    ASSERT_NO_FATAL_FAILURE(expectTombstone(table, "beta"));
    ASSERT_NO_FATAL_FAILURE(expectGet(table, "gamma", "three"));
    ASSERT_NO_FATAL_FAILURE(expectMissing(table, "alpha"));
    ASSERT_NO_FATAL_FAILURE(expectMissing(table, "zulu"));

    SSTableIterator cursor(sstablePath);
    ASSERT_TRUE(cursor.valid());
    ASSERT_NO_FATAL_FAILURE(expectRecord(cursor.current(), "beta", Type::TOMBSTONE, ""));
    cursor.advance();
    ASSERT_TRUE(cursor.valid());
    ASSERT_NO_FATAL_FAILURE(expectRecord(cursor.current(), "gamma", Type::VALUE, "three"));
    cursor.advance();
    EXPECT_FALSE(cursor.valid());
}

TEST(SSTableTest, AddRecordToFileReturnsPublishedFileSize)
{
    const std::filesystem::path root("sstable_tests_add_record_size");
    const ScopedPathCleanup cleanup(root);
    ASSERT_TRUE(std::filesystem::create_directories(root));
    const std::filesystem::path sstablePath = root / "sst_0.sst";
    std::vector<Record> records{
        {"alpha", Type::VALUE, "one"},
        {"beta", Type::TOMBSTONE, ""},
    };

    uint64_t reportedSize = 0;
    ASSERT_NO_THROW(reportedSize = SSTable::addRecordToFile(records, sstablePath));

    ASSERT_TRUE(std::filesystem::is_regular_file(sstablePath));
    EXPECT_EQ(std::filesystem::file_size(sstablePath), reportedSize);
}

TEST(SSTableTest, AddRecordToFileRejectsEmptySpan)
{
    const std::filesystem::path root("sstable_tests_add_record_empty");
    const ScopedPathCleanup cleanup(root);
    std::filesystem::create_directories(root);
    const auto sstablePath = root / "sst_0.sst";

    EXPECT_THROW(SSTable::addRecordToFile(std::span<Record>{}, sstablePath), std::runtime_error);
    EXPECT_FALSE(std::filesystem::exists(sstablePath));
}

TEST(SSTableTest, CleanupOrphanedTempsRemovesTemporaryFile)
{
    const std::filesystem::path root("sstable_tests_cleanup_tmp");
    const std::filesystem::path tempPath = root / "foo.sst.tmp";
    const ScopedPathCleanup cleanup(root);
    std::filesystem::create_directories(root);
    ASSERT_NO_FATAL_FAILURE(writeFile(tempPath, "junk"));

    ASSERT_NO_THROW(SSTable::cleanupOrphanedTemps(root));

    EXPECT_FALSE(std::filesystem::exists(tempPath));
}

TEST(SSTableTest, CleanupOrphanedTempsKeepsSSTableFiles)
{
    const std::filesystem::path walPath("sstable_tests_cleanup_keep.wal");
    const std::filesystem::path root("sstable_tests_cleanup_keep");
    const std::filesystem::path tempPath = root / "foo.sst.tmp";
    const std::filesystem::path sstablePath = root / "bar.sst";
    const ScopedPathCleanup cleanup({walPath, root});
    std::filesystem::create_directories(root);
    ASSERT_NO_FATAL_FAILURE(writeFile(tempPath, "junk"));

    {
        MemTable memTable(walPath.string());
        ASSERT_NO_FATAL_FAILURE(expectPut(memTable, "keep", "me"));

        ASSERT_NO_THROW(SSTable::build(memTable, sstablePath));
    }

    ASSERT_NO_THROW(SSTable::cleanupOrphanedTemps(root));

    EXPECT_FALSE(std::filesystem::exists(tempPath));
    ASSERT_TRUE(std::filesystem::exists(sstablePath));

    const SSTable sstable(sstablePath);
    ASSERT_NO_FATAL_FAILURE(expectGet(sstable, "keep", "me"));
}

TEST(SSTableTest, CleanupOrphanedTempsAcceptsEmptyDirectory)
{
    const std::filesystem::path root("sstable_tests_cleanup_empty");
    const ScopedPathCleanup cleanup(root);
    std::filesystem::create_directories(root);

    ASSERT_NO_THROW(SSTable::cleanupOrphanedTemps(root));

    EXPECT_TRUE(std::filesystem::exists(root));
}

TEST(SSTableTest, PreservesDelimiterAndEmptyFields)
{
    const std::filesystem::path walPath("sstable_tests_raw_fields.wal");
    const std::filesystem::path sstablePath("sstable_tests_raw_fields.sst");
    const ScopedPathCleanup cleanup({walPath, sstablePath});

    {
        MemTable memTable(walPath.string());
        ASSERT_NO_FATAL_FAILURE(expectPut(memTable, "a,b=c", "v=1,ok"));
        ASSERT_NO_FATAL_FAILURE(expectPut(memTable, "", "empty-key"));
        ASSERT_NO_FATAL_FAILURE(expectPut(memTable, "empty-value", ""));
        ASSERT_NO_FATAL_FAILURE(expectPut(memTable, "line\nkey", "line\nvalue\r\n"));

        ASSERT_NO_THROW(SSTable::build(memTable, sstablePath));
    }

    const SSTable sstable(sstablePath);
    ASSERT_NO_FATAL_FAILURE(expectGet(sstable, "a,b=c", "v=1,ok"));
    ASSERT_NO_FATAL_FAILURE(expectGet(sstable, "", "empty-key"));
    ASSERT_NO_FATAL_FAILURE(expectGet(sstable, "empty-value", ""));
    ASSERT_NO_FATAL_FAILURE(expectGet(sstable, "line\nkey", "line\nvalue\r\n"));
}

TEST(SSTableTest, SparseIndexLocatesKeysAcrossMultipleBlocks)
{
    const std::filesystem::path root("sstable_tests_sparse_index_blocks");
    const ScopedPathCleanup cleanup(root);
    const std::filesystem::path walPath = root / "source.wal";
    const std::filesystem::path sstablePath = root / "table.sst";
    constexpr size_t entryCount = 12;
    constexpr size_t valueSize = 1'500;
    constexpr uint64_t recordSize = 9 + 6 + valueSize;
    std::filesystem::create_directories(root);

    {
        MemTable memTable(walPath.string());
        for (size_t i = 0; i < entryCount; ++i)
            ASSERT_NO_FATAL_FAILURE(expectPut(memTable, multiBlockKey(i), std::string(valueSize, 'a' + i)));

        ASSERT_NO_THROW(SSTable::build(memTable, sstablePath));
    }

    std::ifstream in(sstablePath, std::ios::binary);
    ASSERT_TRUE(in.is_open());
    in.seekg(-24, std::ios::end);

    uint64_t recordsSize = 0;
    uint64_t bloomSize = 0;
    uint64_t indexSize = 0;
    ASSERT_TRUE(in.read(reinterpret_cast<char*>(&recordsSize), sizeof(recordsSize)));
    ASSERT_TRUE(in.read(reinterpret_cast<char*>(&bloomSize), sizeof(bloomSize)));
    ASSERT_TRUE(in.read(reinterpret_cast<char*>(&indexSize), sizeof(indexSize)));

    EXPECT_EQ(entryCount * recordSize, recordsSize);
    EXPECT_EQ(4 * (sizeof(uint32_t) + 6 + sizeof(uint64_t)), indexSize);

    const std::array expectedIndices{
        std::pair{multiBlockKey(0), uint64_t{0}},
        std::pair{multiBlockKey(3), 3 * recordSize},
        std::pair{multiBlockKey(6), 6 * recordSize},
        std::pair{multiBlockKey(9), 9 * recordSize},
    };
    in.seekg(recordsSize + bloomSize, std::ios::beg);
    for (const auto& [expectedKey, expectedOffset] : expectedIndices)
    {
        uint32_t keySize = 0;
        uint64_t offset = 0;
        ASSERT_TRUE(in.read(reinterpret_cast<char*>(&keySize), sizeof(keySize)));
        std::string key(keySize, '\0');
        ASSERT_TRUE(in.read(key.data(), keySize));
        ASSERT_TRUE(in.read(reinterpret_cast<char*>(&offset), sizeof(offset)));

        EXPECT_EQ(expectedKey, key);
        EXPECT_EQ(expectedOffset, offset);
    }

    const SSTable sstable(sstablePath);
    for (const size_t index : {0, 2, 3, 6, 9, 11})
    {
        ASSERT_NO_FATAL_FAILURE(expectGet(sstable, multiBlockKey(index), std::string(valueSize, 'a' + index)));
    }
    expectMissing(sstable, "key-025");
    expectMissing(sstable, "key-z");

    BloomFilter mirrorFilter(entryCount, 0.01);
    for (size_t i = 0; i < entryCount; ++i)
        mirrorFilter.add(multiBlockKey(i));

    std::string falsePositiveBeforeFirst;
    for (size_t i = 0; i < 100'000 && falsePositiveBeforeFirst.empty(); ++i)
    {
        auto candidate = "aaa-" + std::to_string(i);
        if (mirrorFilter.mightContain(candidate))
            falsePositiveBeforeFirst = std::move(candidate);
    }

    ASSERT_FALSE(falsePositiveBeforeFirst.empty());
    ASSERT_LT(falsePositiveBeforeFirst, multiBlockKey(0));
    expectMissing(sstable, falsePositiveBeforeFirst);
}

TEST(SSTableTest, ConstructorCachesSparseIndexForSubsequentReads)
{
    const std::filesystem::path root("sstable_tests_cached_sparse_index");
    const ScopedPathCleanup cleanup(root);
    const std::filesystem::path walPath = root / "source.wal";
    const std::filesystem::path sstablePath = root / "table.sst";
    constexpr size_t entryCount = 12;
    constexpr size_t valueSize = 1'500;
    std::filesystem::create_directories(root);

    {
        MemTable memTable(walPath.string());
        for (size_t i = 0; i < entryCount; ++i)
            ASSERT_NO_FATAL_FAILURE(expectPut(memTable, multiBlockKey(i), std::string(valueSize, 'a' + i)));

        ASSERT_NO_THROW(SSTable::build(memTable, sstablePath));
    }

    const SSTable sstable(sstablePath);

    uint64_t recordsSize = 0;
    uint64_t bloomSize = 0;
    uint64_t indexSize = 0;
    {
        std::ifstream input(sstablePath, std::ios::binary);
        ASSERT_TRUE(input.is_open());
        input.seekg(-24, std::ios::end);
        ASSERT_TRUE(input.read(reinterpret_cast<char*>(&recordsSize), sizeof(recordsSize)));
        ASSERT_TRUE(input.read(reinterpret_cast<char*>(&bloomSize), sizeof(bloomSize)));
        ASSERT_TRUE(input.read(reinterpret_cast<char*>(&indexSize), sizeof(indexSize)));
    }
    ASSERT_GT(indexSize, 0);

    // Removing the on-disk index after construction is a deterministic probe: the records and Bloom filter remain
    // readable, so a lookup in a later block succeeds only when the sparse index was retained in memory.
    ASSERT_NO_THROW(std::filesystem::resize_file(sstablePath, recordsSize + bloomSize));
    ASSERT_EQ(recordsSize + bloomSize, std::filesystem::file_size(sstablePath));
    ASSERT_NO_FATAL_FAILURE(expectGet(sstable, multiBlockKey(9), std::string(valueSize, 'a' + 9)));
}

TEST(SSTableTest, SparseIndexPreservesTombstoneInLaterBlock)
{
    const std::filesystem::path root("sstable_tests_sparse_index_tombstone");
    const ScopedPathCleanup cleanup(root);
    const std::filesystem::path walPath = root / "source.wal";
    const std::filesystem::path sstablePath = root / "table.sst";
    constexpr size_t entryCount = 12;
    constexpr size_t valueSize = 1'500;
    std::filesystem::create_directories(root);

    {
        MemTable memTable(walPath.string());
        for (size_t i = 0; i < entryCount; ++i)
            ASSERT_NO_FATAL_FAILURE(expectPut(memTable, multiBlockKey(i), std::string(valueSize, 'a' + i)));
        ASSERT_TRUE(memTable.remove(multiBlockKey(7)));

        ASSERT_NO_THROW(SSTable::build(memTable, sstablePath));
    }

    const SSTable sstable(sstablePath);
    ASSERT_NO_FATAL_FAILURE(expectGet(sstable, multiBlockKey(6), std::string(valueSize, 'a' + 6)));
    ASSERT_NO_FATAL_FAILURE(expectTombstone(sstable, multiBlockKey(7)));
    ASSERT_NO_FATAL_FAILURE(expectGet(sstable, multiBlockKey(8), std::string(valueSize, 'a' + 8)));
}

TEST(SSTableTest, SparseIndexStopsAtDeclaredSizeBeforeFooter)
{
    const std::filesystem::path root("sstable_tests_sparse_index_footer_boundary");
    const ScopedPathCleanup cleanup(root);
    const std::filesystem::path walPath = root / "source.wal";
    const std::filesystem::path sstablePath = root / "table.sst";
    std::filesystem::create_directories(root);

    {
        MemTable memTable(walPath.string());
        ASSERT_NO_FATAL_FAILURE(expectPut(memTable, "zzz", ""));
        ASSERT_NO_THROW(SSTable::build(memTable, sstablePath));
    }

    std::ifstream in(sstablePath, std::ios::binary);
    ASSERT_TRUE(in.is_open());
    in.seekg(-24, std::ios::end);

    uint64_t recordsSize = 0;
    uint64_t bloomSize = 0;
    uint64_t indexSize = 0;
    ASSERT_TRUE(in.read(reinterpret_cast<char*>(&recordsSize), sizeof(recordsSize)));
    ASSERT_TRUE(in.read(reinterpret_cast<char*>(&bloomSize), sizeof(bloomSize)));
    ASSERT_TRUE(in.read(reinterpret_cast<char*>(&indexSize), sizeof(indexSize)));
    EXPECT_EQ(12, recordsSize);
    EXPECT_GT(bloomSize, 0);
    EXPECT_EQ(sizeof(uint32_t) + 3 + sizeof(uint64_t), indexSize);

    const SSTable sstable(sstablePath);
    ASSERT_NO_FATAL_FAILURE(expectGet(sstable, "zzz", ""));
}

TEST(SSTableTest, ConstructorRejectsMissingFile)
{
    const std::filesystem::path sstablePath("sstable_tests_missing.sst");
    const ScopedPathCleanup cleanup(sstablePath);

    EXPECT_THROW(SSTable sstable(sstablePath), std::runtime_error);
}
