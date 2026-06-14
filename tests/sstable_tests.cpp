#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>

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
    ASSERT_TRUE(table.get(key, actual)) << "expected key to exist: " << key;
    EXPECT_EQ(expected, actual) << "unexpected value for key: " << key;
}

void expectMissing(const SSTable &table, const std::string &key)
{
    std::string actual = "unchanged";
    EXPECT_FALSE(table.get(key, actual)) << "expected key to be missing: " << key;
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

TEST(SSTableTest, BuildRejectsExistingFileWithoutOverwriting)
{
    const std::filesystem::path walPath("sstable_tests_existing.wal");
    const std::filesystem::path sstablePath("sstable_tests_existing.sst");
    std::filesystem::remove(walPath);
    std::filesystem::remove(sstablePath);
    ASSERT_NO_FATAL_FAILURE(writeFile(sstablePath, "sentinel"));

    {
        MemTable memTable(walPath.string());
        ASSERT_NO_FATAL_FAILURE(expectPut(memTable, "k", "v"));

        EXPECT_THROW(SSTable::build(memTable, sstablePath), std::runtime_error);
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(sstablePath, "sentinel"));

    std::filesystem::remove(sstablePath);
    std::filesystem::remove(walPath);
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
