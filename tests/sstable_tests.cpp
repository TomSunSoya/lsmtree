#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "MemTable.h"
#include "SSTable.h"

namespace
{
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

void expectFileHexDump(const std::filesystem::path &path, const std::string &expected)
{
    std::string content;
    readFile(path, content);
    if (::testing::Test::HasFatalFailure())
        return;

    EXPECT_EQ(expected, hexDump(content)) << "unexpected file bytes in " << path;
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

TEST(SSTableTest, CursorIteratesRecordsInTableOrder)
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

    Cursor cursor(sstablePath);
    ASSERT_TRUE(cursor.valid());
    ASSERT_NO_FATAL_FAILURE(expectRecord(cursor.current(), "alpha", Type::VALUE, "one"));

    cursor.advance();
    ASSERT_TRUE(cursor.valid());
    ASSERT_NO_FATAL_FAILURE(expectRecord(cursor.current(), "beta", Type::VALUE, "two"));

    cursor.advance();
    EXPECT_FALSE(cursor.valid());

    std::filesystem::remove(sstablePath);
    std::filesystem::remove(walPath);
}

TEST(SSTableTest, CursorExposesTombstoneRecords)
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

    Cursor cursor(sstablePath);
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

TEST(SSTableTest, CursorRejectsMissingFile)
{
    const std::filesystem::path sstablePath("sstable_tests_cursor_missing.sst");
    std::filesystem::remove(sstablePath);

    EXPECT_THROW(Cursor cursor(sstablePath), std::runtime_error);
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

TEST(SSTableTest, MaxFileByNameReturnsNextSSTableNumberAndIgnoresNonMatches)
{
    const std::filesystem::path root("sstable_tests_max_file_by_name");
    const ScopedPathCleanup cleanup(root);
    std::filesystem::create_directories(root / "subdir");
    ASSERT_NO_FATAL_FAILURE(writeFile(root / "sst_0.sst", ""));
    ASSERT_NO_FATAL_FAILURE(writeFile(root / "sst_10.sst", ""));
    ASSERT_NO_FATAL_FAILURE(writeFile(root / "sst_2.sst.tmp", ""));
    ASSERT_NO_FATAL_FAILURE(writeFile(root / "sst_old.sst", ""));
    ASSERT_NO_FATAL_FAILURE(writeFile(root / "wal_20.wal", ""));
    ASSERT_NO_FATAL_FAILURE(writeFile(root / "subdir" / "sst_99.sst", ""));

    EXPECT_EQ(11, maxFileByName(root));
    EXPECT_EQ(0, maxFileByName(root / "missing"));
}

TEST(SSTableTest, BuildWritesExpectedLittleEndianBytes)
{
    const std::filesystem::path walPath("sstable_tests_exact_bytes.wal");
    const std::filesystem::path sstablePath("sstable_tests_exact_bytes.sst");
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

    EXPECT_EQ(
        "00 01 00 00 00 02 00 00 00 61 78 79 "
        "00 04 00 00 00 00 00 00 00 6c 6f 6e 67",
        hexDump(content));

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
    ASSERT_NO_FATAL_FAILURE(expectFileHexDump(
        sstablePath,
        "00 06 00 00 00 05 00 00 00 73 74 61 62 6c 65 74 61 62 6c 65"));

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
    ASSERT_NO_FATAL_FAILURE(expectFileHexDump(
        sstablePath,
        "00 05 00 00 00 05 00 00 00 66 72 65 73 68 76 61 6c 75 65"));

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

    ASSERT_NO_FATAL_FAILURE(expectFileHexDump(
        sstablePath,
        "00 08 00 00 00 05 00 00 00 6f 72 69 67 69 6e 61 6c 76 61 6c 75 65"));

    {
        MemTable memTable(replacementWalPath.string());
        ASSERT_NO_FATAL_FAILURE(expectPut(memTable, "replacement", "data"));

        EXPECT_THROW(SSTable::build(memTable, sstablePath), std::runtime_error);
    }

    EXPECT_FALSE(std::filesystem::exists(tempPath)) << "rejecting an existing target should not publish or leave a .tmp file";
    ASSERT_NO_FATAL_FAILURE(expectFileHexDump(
        sstablePath,
        "00 08 00 00 00 05 00 00 00 6f 72 69 67 69 6e 61 6c 76 61 6c 75 65"));

    std::filesystem::remove(tempPath);
    std::filesystem::remove(sstablePath);
    std::filesystem::remove(originalWalPath);
    std::filesystem::remove(replacementWalPath);
}

TEST(SSTableTest, MergePublishesNextNumberedTableWithSortedRecords)
{
    const std::filesystem::path root("sstable_tests_merge_sorted");
    const ScopedPathCleanup cleanup(root);
    std::filesystem::create_directories(root);

    {
        MemTable memTable((root / "wal_0.wal").string());
        ASSERT_NO_FATAL_FAILURE(expectPut(memTable, "gamma", "three"));
        ASSERT_NO_THROW(SSTable::build(memTable, root / "sst_0.sst"));
    }
    {
        MemTable memTable((root / "wal_1.wal").string());
        ASSERT_NO_FATAL_FAILURE(expectPut(memTable, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectPut(memTable, "beta", "two"));
        ASSERT_NO_THROW(SSTable::build(memTable, root / "sst_1.sst"));
    }

    ASSERT_NO_THROW(SSTable::merge(root));

    const std::filesystem::path mergedPath = root / "sst_2.sst";
    ASSERT_TRUE(std::filesystem::is_regular_file(mergedPath));

    Cursor cursor(mergedPath);
    ASSERT_TRUE(cursor.valid());
    ASSERT_NO_FATAL_FAILURE(expectRecord(cursor.current(), "alpha", Type::VALUE, "one"));
    cursor.advance();
    ASSERT_TRUE(cursor.valid());
    ASSERT_NO_FATAL_FAILURE(expectRecord(cursor.current(), "beta", Type::VALUE, "two"));
    cursor.advance();
    ASSERT_TRUE(cursor.valid());
    ASSERT_NO_FATAL_FAILURE(expectRecord(cursor.current(), "gamma", Type::VALUE, "three"));
    cursor.advance();
    EXPECT_FALSE(cursor.valid());
}

TEST(SSTableTest, MergeKeepsNewestDuplicateAndContinuesOlderCursor)
{
    const std::filesystem::path root("sstable_tests_merge_duplicate_continues");
    const ScopedPathCleanup cleanup(root);
    std::filesystem::create_directories(root);

    {
        MemTable memTable((root / "wal_0.wal").string());
        ASSERT_NO_FATAL_FAILURE(expectPut(memTable, "alpha", "old"));
        ASSERT_NO_FATAL_FAILURE(expectPut(memTable, "beta", "kept"));
        ASSERT_NO_THROW(SSTable::build(memTable, root / "sst_0.sst"));
    }
    {
        MemTable memTable((root / "wal_1.wal").string());
        ASSERT_NO_FATAL_FAILURE(expectPut(memTable, "alpha", "new"));
        ASSERT_NO_THROW(SSTable::build(memTable, root / "sst_1.sst"));
    }

    ASSERT_NO_THROW(SSTable::merge(root));

    const SSTable merged(root / "sst_2.sst");
    ASSERT_NO_FATAL_FAILURE(expectGet(merged, "alpha", "new"));
    ASSERT_NO_FATAL_FAILURE(expectGet(merged, "beta", "kept"));
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
    ASSERT_NO_FATAL_FAILURE(expectFileHexDump(
        sstablePath,
        "00 04 00 00 00 02 00 00 00 6b 65 65 70 6d 65"));

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

TEST(SSTableTest, MissingFileReadsAsEmptyTable)
{
    const std::filesystem::path sstablePath("sstable_tests_missing.sst");
    std::filesystem::remove(sstablePath);

    const SSTable sstable(sstablePath);
    expectMissing(sstable, "missing");
}
