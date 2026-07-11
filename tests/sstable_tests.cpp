#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "MemTable.h"
#include "SSTable.h"

namespace
{
std::string multiBlockKey(const size_t index)
{
    return "key-" + std::string(index < 10 ? "0" : "") + std::to_string(index);
}

void expectPut(MemTable &table, const std::string &key, const std::string &value)
{
    ASSERT_TRUE(table.put(key, value)) << "expected put to succeed for key: " << key;
}

void expectGet(const SSTable &table, const std::string &key, const std::string &expected)
{
    std::string actual;
    ASSERT_EQ(Result::VALUE, table.get(key, actual)) << "expected key to exist: " << key;
    EXPECT_EQ(expected, actual) << "unexpected value for key: " << key;
}

void expectMissing(const SSTable &table, const std::string &key)
{
    std::string actual = "unchanged";
    EXPECT_EQ(Result::ABSENT, table.get(key, actual)) << "expected key to be missing: " << key;
}

void expectTombstone(const SSTable &table, const std::string &key)
{
    std::string actual = "unchanged";
    EXPECT_EQ(Result::TOMBSTONE, table.get(key, actual)) << "expected tombstone for key: " << key;
}

void readFile(const std::filesystem::path &path, std::string &content)
{
    std::ifstream in(path, std::ios::binary);
    ASSERT_TRUE(in.is_open()) << "expected file to exist: " << path;

    content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeFile(const std::filesystem::path &path, const std::string &content)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open()) << "expected file to open for writing: " << path;

    out << content;
    ASSERT_TRUE(out.good()) << "expected file write to succeed: " << path;
}

void expectFileContent(const std::filesystem::path &path, const std::string &expected)
{
    std::string actual;
    readFile(path, actual);
    if (::testing::Test::HasFatalFailure())
        return;

    EXPECT_EQ(expected, actual) << "unexpected file content in " << path;
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

void expectRecord(const Record &record, const std::string &key, const Type type, const std::string &value)
{
    EXPECT_EQ(key, record.key);
    EXPECT_EQ(type, record.type);
    EXPECT_EQ(value, record.value);
}

class ScopedPathCleanup
{
public:
    explicit ScopedPathCleanup(std::filesystem::path path) : path(std::move(path))
    {
        std::filesystem::remove_all(this->path);
    }

    ~ScopedPathCleanup()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

private:
    std::filesystem::path path;
};
}

TEST(SSTableTest, BuildAndReadRecordsFromMemTable)
{
    const std::filesystem::path walPath("sstable_tests_build_read.wal");
    const std::filesystem::path sstablePath("sstable_tests_build_read.sst");
    std::filesystem::remove(walPath);
    std::filesystem::remove(sstablePath);

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

    std::filesystem::remove(sstablePath);
    std::filesystem::remove(walPath);
}

TEST(SSTableTest, BuildPersistsTombstoneAndKeepsScanningUnrelatedKeys)
{
    const std::filesystem::path walPath("sstable_tests_tombstone.wal");
    const std::filesystem::path sstablePath("sstable_tests_tombstone.sst");
    std::filesystem::remove(walPath);
    std::filesystem::remove(sstablePath);

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

    std::filesystem::remove(sstablePath);
    std::filesystem::remove(walPath);
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
    std::filesystem::remove(walPath);
    std::filesystem::remove(sstablePath);

    {
        MemTable memTable(walPath.string());
        ASSERT_NO_FATAL_FAILURE(expectPut(memTable, "beta", "two"));
        ASSERT_NO_FATAL_FAILURE(expectPut(memTable, "alpha", "one"));

        ASSERT_NO_THROW(SSTable::build(memTable, sstablePath));
    }

    SSTableIterator concreteIterator(sstablePath);
    Iterator &iterator = concreteIterator;
    ASSERT_TRUE(iterator.valid());
    ASSERT_NO_FATAL_FAILURE(expectRecord(iterator.current(), "alpha", Type::VALUE, "one"));

    iterator.advance();
    ASSERT_TRUE(iterator.valid());
    ASSERT_NO_FATAL_FAILURE(expectRecord(iterator.current(), "beta", Type::VALUE, "two"));

    iterator.advance();
    EXPECT_FALSE(iterator.valid());

    std::filesystem::remove(sstablePath);
    std::filesystem::remove(walPath);
}

TEST(SSTableTest, IteratorExposesTombstoneRecords)
{
    const std::filesystem::path walPath("sstable_tests_cursor_tombstone.wal");
    const std::filesystem::path sstablePath("sstable_tests_cursor_tombstone.sst");
    std::filesystem::remove(walPath);
    std::filesystem::remove(sstablePath);

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

    std::filesystem::remove(sstablePath);
    std::filesystem::remove(walPath);
}

TEST(SSTableTest, IteratorRejectsMissingFile)
{
    const std::filesystem::path sstablePath("sstable_tests_cursor_missing.sst");
    std::filesystem::remove(sstablePath);

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
    constexpr size_t expectedRecordsSize = 25;
    std::filesystem::remove(walPath);
    std::filesystem::remove(sstablePath);

    {
        MemTable memTable(walPath.string());
        ASSERT_NO_FATAL_FAILURE(expectPut(memTable, "long", ""));
        ASSERT_NO_FATAL_FAILURE(expectPut(memTable, "a", "xy"));

        ASSERT_NO_THROW(SSTable::build(memTable, sstablePath));
    }

    std::string content;
    ASSERT_NO_FATAL_FAILURE(readFile(sstablePath, content));

    ASSERT_GE(content.size(), expectedRecordsSize);
    EXPECT_EQ(
        "00 01 00 00 00 02 00 00 00 61 78 79 "
        "00 04 00 00 00 00 00 00 00 6c 6f 6e 67",
        hexDump(std::string_view(content).substr(0, expectedRecordsSize)));

    std::filesystem::remove(sstablePath);
    std::filesystem::remove(walPath);
}

TEST(SSTableTest, BuildCreatesParentDirectories)
{
    const std::filesystem::path walPath("sstable_tests_nested.wal");
    const std::filesystem::path root("sstable_tests_nested");
    const std::filesystem::path sstablePath = root / "child" / "table.sst";
    std::filesystem::remove(walPath);
    std::filesystem::remove_all(root);

    {
        MemTable memTable(walPath.string());
        ASSERT_NO_FATAL_FAILURE(expectPut(memTable, "parent", "created"));

        ASSERT_NO_THROW(SSTable::build(memTable, sstablePath));
    }

    ASSERT_TRUE(std::filesystem::exists(sstablePath)) << "expected SSTable file in nested directory";

    const SSTable sstable(sstablePath);
    ASSERT_NO_FATAL_FAILURE(expectGet(sstable, "parent", "created"));

    std::filesystem::remove_all(root);
    std::filesystem::remove(walPath);
}

TEST(SSTableTest, BuildDoesNotLeaveTemporaryFileAfterSuccess)
{
    const std::filesystem::path walPath("sstable_tests_no_tmp_after_success.wal");
    const std::filesystem::path sstablePath("sstable_tests_no_tmp_after_success.sst");
    const std::filesystem::path tempPath(sstablePath.string() + ".tmp");
    std::filesystem::remove(walPath);
    std::filesystem::remove(sstablePath);
    std::filesystem::remove(tempPath);

    {
        MemTable memTable(walPath.string());
        ASSERT_NO_FATAL_FAILURE(expectPut(memTable, "stable", "table"));

        ASSERT_NO_THROW(SSTable::build(memTable, sstablePath));
    }

    ASSERT_TRUE(std::filesystem::exists(sstablePath));
    EXPECT_FALSE(std::filesystem::exists(tempPath)) << "successful build should publish by rename and leave no .tmp file";

    const SSTable sstable(sstablePath);
    ASSERT_NO_FATAL_FAILURE(expectGet(sstable, "stable", "table"));

    std::filesystem::remove(tempPath);
    std::filesystem::remove(sstablePath);
    std::filesystem::remove(walPath);
}

TEST(SSTableTest, BuildOverwritesStaleTemporaryFile)
{
    const std::filesystem::path walPath("sstable_tests_stale_tmp.wal");
    const std::filesystem::path sstablePath("sstable_tests_stale_tmp.sst");
    const std::filesystem::path tempPath(sstablePath.string() + ".tmp");
    std::filesystem::remove(walPath);
    std::filesystem::remove(sstablePath);
    std::filesystem::remove(tempPath);
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

    std::filesystem::remove(tempPath);
    std::filesystem::remove(sstablePath);
    std::filesystem::remove(walPath);
}

TEST(SSTableTest, BuildRejectsExistingFileWithoutOverwriting)
{
    const std::filesystem::path originalWalPath("sstable_tests_existing_original.wal");
    const std::filesystem::path replacementWalPath("sstable_tests_existing_replacement.wal");
    const std::filesystem::path sstablePath("sstable_tests_existing.sst");
    const std::filesystem::path tempPath(sstablePath.string() + ".tmp");
    std::filesystem::remove(originalWalPath);
    std::filesystem::remove(replacementWalPath);
    std::filesystem::remove(sstablePath);
    std::filesystem::remove(tempPath);

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

    EXPECT_FALSE(std::filesystem::exists(tempPath)) << "rejecting an existing target should not publish or leave a .tmp file";
    std::string contentAfterRejectedBuild;
    ASSERT_NO_FATAL_FAILURE(readFile(sstablePath, contentAfterRejectedBuild));
    EXPECT_EQ(originalContent, contentAfterRejectedBuild);

    const SSTable sstable(sstablePath);
    ASSERT_NO_FATAL_FAILURE(expectGet(sstable, "original", "value"));

    std::filesystem::remove(tempPath);
    std::filesystem::remove(sstablePath);
    std::filesystem::remove(originalWalPath);
    std::filesystem::remove(replacementWalPath);
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
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    ASSERT_NO_FATAL_FAILURE(writeFile(tempPath, "junk"));

    ASSERT_NO_THROW(SSTable::cleanupOrphanedTemps(root));

    EXPECT_FALSE(std::filesystem::exists(tempPath));

    std::filesystem::remove_all(root);
}

TEST(SSTableTest, CleanupOrphanedTempsKeepsSSTableFiles)
{
    const std::filesystem::path walPath("sstable_tests_cleanup_keep.wal");
    const std::filesystem::path root("sstable_tests_cleanup_keep");
    const std::filesystem::path tempPath = root / "foo.sst.tmp";
    const std::filesystem::path sstablePath = root / "bar.sst";
    std::filesystem::remove(walPath);
    std::filesystem::remove_all(root);
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

    std::filesystem::remove_all(root);
    std::filesystem::remove(walPath);
}

TEST(SSTableTest, CleanupOrphanedTempsAcceptsEmptyDirectory)
{
    const std::filesystem::path root("sstable_tests_cleanup_empty");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    ASSERT_NO_THROW(SSTable::cleanupOrphanedTemps(root));

    EXPECT_TRUE(std::filesystem::exists(root));

    std::filesystem::remove_all(root);
}

TEST(SSTableTest, PreservesDelimiterAndEmptyFields)
{
    const std::filesystem::path walPath("sstable_tests_raw_fields.wal");
    const std::filesystem::path sstablePath("sstable_tests_raw_fields.sst");
    std::filesystem::remove(walPath);
    std::filesystem::remove(sstablePath);

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

    std::filesystem::remove(sstablePath);
    std::filesystem::remove(walPath);
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
    for (const auto &[expectedKey, expectedOffset] : expectedIndices)
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
        ASSERT_NO_FATAL_FAILURE(expectGet(
            sstable,
            multiBlockKey(index),
            std::string(valueSize, 'a' + index)));
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
    std::filesystem::remove(sstablePath);

    EXPECT_THROW(SSTable sstable(sstablePath), std::runtime_error);
}
