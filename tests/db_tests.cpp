#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <gtest/gtest.h>

#include "DB.h"

namespace
{
void expectPut(DB &db, const std::string &key, const std::string &value)
{
    ASSERT_TRUE(db.put(key, value)) << "expected put to succeed for key: " << key;
}

void expectGet(const DB &db, const std::string &key, const std::string &expected)
{
    std::string actual;
    ASSERT_TRUE(db.get(key, actual)) << "expected key to exist: " << key;
    EXPECT_EQ(expected, actual) << "unexpected value for key: " << key;
}

void expectMissing(const DB &db, const std::string &key)
{
    std::string actual = "unchanged";
    EXPECT_FALSE(db.get(key, actual)) << "expected key to be missing: " << key;
}

void writeFile(const std::filesystem::path &path, const std::string &content)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open()) << "expected file to open for writing: " << path;

    out << content;
    ASSERT_TRUE(out.good()) << "expected file write to succeed: " << path;
}

void readFile(const std::filesystem::path &path, std::string &content)
{
    std::ifstream in(path, std::ios::binary);
    ASSERT_TRUE(in.is_open()) << "expected file to exist: " << path;

    content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void expectFileContent(const std::filesystem::path &path, const std::string &expected)
{
    std::string actual;
    readFile(path, actual);
    if (::testing::Test::HasFatalFailure())
        return;

    EXPECT_EQ(expected, actual) << "unexpected file content in " << path;
}
}

TEST(DBTest, ConstructorCreatesDataDirectoryAndWalFile)
{
    const std::filesystem::path root("db_tests_create_data_dir");
    std::filesystem::remove_all(root);

    {
        const DB db(root);

        EXPECT_TRUE(std::filesystem::is_directory(root));
        EXPECT_TRUE(std::filesystem::is_directory(root / "wal"));
        EXPECT_TRUE(std::filesystem::is_regular_file(root / "wal" / "wal1.wal"));
    }

    std::filesystem::remove_all(root);
}

TEST(DBTest, ConstructorRejectsExistingNonDirectoryPath)
{
    const std::filesystem::path root("db_tests_data_dir_is_file");
    std::filesystem::remove_all(root);
    std::filesystem::remove(root);
    ASSERT_NO_FATAL_FAILURE(writeFile(root, "not a directory"));

    EXPECT_THROW(DB db(root), std::invalid_argument);

    std::filesystem::remove(root);
}

TEST(DBTest, PutAndGetUseActiveMemTable)
{
    const std::filesystem::path root("db_tests_put_get");
    std::filesystem::remove_all(root);

    {
        DB db(root);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "beta", "two"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "beta", "two"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "alpha", "one"));
        expectMissing(db, "gamma");
    }

    std::filesystem::remove_all(root);
}

TEST(DBTest, PutUpdatesExistingKey)
{
    const std::filesystem::path root("db_tests_update_existing");
    std::filesystem::remove_all(root);

    {
        DB db(root);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "key", "old"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "key", "new"));

        ASSERT_NO_FATAL_FAILURE(expectGet(db, "key", "new"));
    }

    std::filesystem::remove_all(root);
}

TEST(DBTest, PutAndGetPreserveEmptyKeysAndValues)
{
    const std::filesystem::path root("db_tests_empty_fields");
    std::filesystem::remove_all(root);

    {
        DB db(root);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "", "empty-key"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "empty-value", ""));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "", ""));

        ASSERT_NO_FATAL_FAILURE(expectGet(db, "", ""));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "empty-value", ""));
    }

    std::filesystem::remove_all(root);
}

TEST(DBTest, ReopensFromWal)
{
    const std::filesystem::path root("db_tests_reopen_from_wal");
    std::filesystem::remove_all(root);

    {
        DB db(root);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "beta", "two"));
    }

    {
        const DB db(root);

        ASSERT_NO_FATAL_FAILURE(expectGet(db, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "beta", "two"));
        expectMissing(db, "gamma");
    }

    std::filesystem::remove_all(root);
}

TEST(DBTest, WritesToExpectedWalPath)
{
    const std::filesystem::path root("db_tests_wal_path");
    const std::filesystem::path walPath = root / "wal" / "wal1.wal";
    std::filesystem::remove_all(root);

    {
        DB db(root);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "beta", "two"));
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(walPath, "5,alpha=3,one\n4,beta=3,two\n"));

    std::filesystem::remove_all(root);
}
