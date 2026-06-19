#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>

#include <gtest/gtest.h>

#include "DB.h"

namespace
{
constexpr uint64_t kManualFlushThreshold = std::numeric_limits<uint64_t>::max();

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
        const DB db(root, kManualFlushThreshold);

        EXPECT_TRUE(std::filesystem::is_directory(root));
        EXPECT_TRUE(std::filesystem::is_directory(root / "wal"));
        EXPECT_TRUE(std::filesystem::is_regular_file(root / "wal" / "wal_0.wal"));
    }

    std::filesystem::remove_all(root);
}

TEST(DBTest, ConstructorRejectsExistingNonDirectoryPath)
{
    const std::filesystem::path root("db_tests_data_dir_is_file");
    std::filesystem::remove_all(root);
    std::filesystem::remove(root);
    ASSERT_NO_FATAL_FAILURE(writeFile(root, "not a directory"));

    EXPECT_THROW(DB db(root, kManualFlushThreshold), std::invalid_argument);

    std::filesystem::remove(root);
}

TEST(DBTest, PutAndGetUseActiveMemTable)
{
    const std::filesystem::path root("db_tests_put_get");
    std::filesystem::remove_all(root);

    {
        DB db(root, kManualFlushThreshold);

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
        DB db(root, kManualFlushThreshold);

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
        DB db(root, kManualFlushThreshold);

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
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "beta", "two"));
    }

    {
        const DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectGet(db, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "beta", "two"));
        expectMissing(db, "gamma");
    }

    std::filesystem::remove_all(root);
}

TEST(DBTest, ReopensFromWalWhenEmptySSTableDirectoryExists)
{
    const std::filesystem::path root("db_tests_reopen_wal_with_empty_sstable_dir");
    std::filesystem::remove_all(root);

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "one"));
    }

    ASSERT_TRUE(std::filesystem::create_directories(root / "sstable"));

    {
        const DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectGet(db, "alpha", "one"));
    }

    std::filesystem::remove_all(root);
}

TEST(DBTest, WritesToExpectedWalPath)
{
    const std::filesystem::path root("db_tests_wal_path");
    const std::filesystem::path walPath = root / "wal" / "wal_0.wal";
    std::filesystem::remove_all(root);

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "beta", "two"));
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(walPath, "P,5,alpha=3,one\nP,4,beta=3,two\n"));

    std::filesystem::remove_all(root);
}

TEST(DBTest, PutDoesNotAutoFlushWhenMemTableSizeEqualsThreshold)
{
    const std::filesystem::path root("db_tests_auto_flush_equal_threshold");
    const std::filesystem::path walPath = root / "wal" / "wal_0.wal";
    const std::filesystem::path sstablePath = root / "sstable" / "sst_0.sst";
    constexpr uint64_t threshold = 9;
    std::filesystem::remove_all(root);

    {
        DB db(root, threshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "one"));

        EXPECT_FALSE(std::filesystem::exists(sstablePath));
        EXPECT_TRUE(std::filesystem::is_regular_file(walPath));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "alpha", "one"));
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(walPath, "P,5,alpha=3,one\n"));

    std::filesystem::remove_all(root);
}

TEST(DBTest, PutAutoFlushesWhenMemTableSizeExceedsThreshold)
{
    const std::filesystem::path root("db_tests_auto_flush_exceeds_threshold");
    const std::filesystem::path oldWalPath = root / "wal" / "wal_0.wal";
    const std::filesystem::path newWalPath = root / "wal" / "wal_1.wal";
    const std::filesystem::path sstablePath = root / "sstable" / "sst_0.sst";
    constexpr uint64_t threshold = 6;
    std::filesystem::remove_all(root);

    {
        DB db(root, threshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "a", "12"));
        EXPECT_FALSE(std::filesystem::exists(sstablePath));
        EXPECT_TRUE(std::filesystem::is_regular_file(oldWalPath));

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "bc", "345"));

        EXPECT_TRUE(std::filesystem::is_regular_file(sstablePath));
        EXPECT_FALSE(std::filesystem::exists(oldWalPath));
        EXPECT_TRUE(std::filesystem::is_regular_file(newWalPath));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "a", "12"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "bc", "345"));

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "d", "4"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "d", "4"));
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(newWalPath, "P,1,d=1,4\n"));

    std::filesystem::remove_all(root);
}

TEST(DBTest, FlushPublishesSSTableAndRotatesWal)
{
    const std::filesystem::path root("db_tests_flush_rotates_wal");
    const std::filesystem::path oldWalPath = root / "wal" / "wal_0.wal";
    const std::filesystem::path newWalPath = root / "wal" / "wal_1.wal";
    const std::filesystem::path sstablePath = root / "sstable" / "sst_0.sst";
    std::filesystem::remove_all(root);

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "one"));
        ASSERT_TRUE(std::filesystem::exists(oldWalPath));

        ASSERT_NO_THROW(db.flush());

        EXPECT_TRUE(std::filesystem::is_regular_file(sstablePath));
        EXPECT_FALSE(std::filesystem::exists(oldWalPath));
        EXPECT_TRUE(std::filesystem::is_regular_file(newWalPath));
    }

    std::filesystem::remove_all(root);
}

TEST(DBTest, GetFallsBackToFlushedSSTable)
{
    const std::filesystem::path root("db_tests_get_from_sstable");
    std::filesystem::remove_all(root);

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "beta", "two"));
        ASSERT_NO_THROW(db.flush());

        ASSERT_NO_FATAL_FAILURE(expectGet(db, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "beta", "two"));
        expectMissing(db, "gamma");
    }

    std::filesystem::remove_all(root);
}

TEST(DBTest, ActiveMemTableOverridesFlushedSSTable)
{
    const std::filesystem::path root("db_tests_active_overrides_sstable");
    std::filesystem::remove_all(root);

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "key", "old"));
        ASSERT_NO_THROW(db.flush());
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "key", "new"));

        ASSERT_NO_FATAL_FAILURE(expectGet(db, "key", "new"));
    }

    std::filesystem::remove_all(root);
}

TEST(DBTest, NewActiveMemTableWritesToNextWalAfterFlush)
{
    const std::filesystem::path root("db_tests_next_wal_after_flush");
    const std::filesystem::path oldWalPath = root / "wal" / "wal_0.wal";
    const std::filesystem::path newWalPath = root / "wal" / "wal_1.wal";
    std::filesystem::remove_all(root);

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "one"));
        ASSERT_NO_THROW(db.flush());
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "beta", "two"));
    }

    EXPECT_FALSE(std::filesystem::exists(oldWalPath));
    ASSERT_NO_FATAL_FAILURE(expectFileContent(newWalPath, "P,4,beta=3,two\n"));

    std::filesystem::remove_all(root);
}

TEST(DBTest, ReopensFromFlushedSSTable)
{
    const std::filesystem::path root("db_tests_reopen_from_sstable");
    std::filesystem::remove_all(root);

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "beta", "two"));
        ASSERT_NO_THROW(db.flush());
    }

    {
        const DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectGet(db, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "beta", "two"));
        expectMissing(db, "gamma");
    }

    std::filesystem::remove_all(root);
}

TEST(DBTest, ReopenContinuesSSTableNumberAfterExistingFiles)
{
    const std::filesystem::path root("db_tests_continue_sstable_number");
    const std::filesystem::path firstSSTablePath = root / "sstable" / "sst_0.sst";
    const std::filesystem::path secondSSTablePath = root / "sstable" / "sst_1.sst";
    const std::filesystem::path nextWalPath = root / "wal" / "wal_2.wal";
    std::filesystem::remove_all(root);

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "one"));
        ASSERT_NO_THROW(db.flush());
    }

    ASSERT_TRUE(std::filesystem::exists(firstSSTablePath));

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "beta", "two"));
        ASSERT_NO_THROW(db.flush());
    }

    EXPECT_TRUE(std::filesystem::is_regular_file(firstSSTablePath));
    EXPECT_TRUE(std::filesystem::is_regular_file(secondSSTablePath));
    EXPECT_TRUE(std::filesystem::is_regular_file(nextWalPath));

    std::filesystem::remove_all(root);
}

TEST(DBTest, ReopenRemovesWalFilesOlderThanCurrentSSTableNumber)
{
    const std::filesystem::path root("db_tests_cleanup_old_wal");
    const std::filesystem::path oldWalPath = root / "wal" / "wal_0.wal";
    const std::filesystem::path currentWalPath = root / "wal" / "wal_1.wal";
    std::filesystem::remove_all(root);

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "one"));
        ASSERT_NO_THROW(db.flush());
    }

    ASSERT_TRUE(std::filesystem::exists(root / "sstable" / "sst_0.sst"));
    ASSERT_NO_FATAL_FAILURE(writeFile(oldWalPath, "P,5,stale=7,ignored\n"));
    ASSERT_NO_FATAL_FAILURE(writeFile(currentWalPath, "P,4,beta=3,two\n"));

    {
        const DB db(root, kManualFlushThreshold);

        EXPECT_FALSE(std::filesystem::exists(oldWalPath));
        EXPECT_TRUE(std::filesystem::is_regular_file(currentWalPath));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "beta", "two"));
        expectMissing(db, "stale");
    }

    std::filesystem::remove_all(root);
}

TEST(DBTest, ReopenKeepsWalFilesThatDoNotMatchOldNumberPattern)
{
    const std::filesystem::path root("db_tests_cleanup_wal_keeps_unrelated");
    const std::filesystem::path staleWalPath = root / "wal" / "wal_0.wal";
    const std::filesystem::path malformedWalPath = root / "wal" / "wal_old.wal";
    const std::filesystem::path wrongSuffixPath = root / "wal" / "wal_0.txt";
    const std::filesystem::path futureWalPath = root / "wal" / "wal_2.wal";
    std::filesystem::remove_all(root);

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "one"));
        ASSERT_NO_THROW(db.flush());
    }

    ASSERT_NO_FATAL_FAILURE(writeFile(staleWalPath, "P,5,stale=7,ignored\n"));
    ASSERT_NO_FATAL_FAILURE(writeFile(malformedWalPath, "kept"));
    ASSERT_NO_FATAL_FAILURE(writeFile(wrongSuffixPath, "kept"));
    ASSERT_NO_FATAL_FAILURE(writeFile(futureWalPath, "kept"));

    {
        const DB db(root, kManualFlushThreshold);

        EXPECT_FALSE(std::filesystem::exists(staleWalPath));
        EXPECT_TRUE(std::filesystem::is_regular_file(malformedWalPath));
        EXPECT_TRUE(std::filesystem::is_regular_file(wrongSuffixPath));
        EXPECT_TRUE(std::filesystem::is_regular_file(futureWalPath));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "alpha", "one"));
    }

    std::filesystem::remove_all(root);
}

TEST(DBTest, ConstructorRemovesOrphanedSSTableTemporaryFiles)
{
    const std::filesystem::path root("db_tests_cleanup_sstable_tmp");
    const std::filesystem::path sstableDir = root / "sstable";
    const std::filesystem::path tempPath = sstableDir / "sst_0.sst.tmp";
    const std::filesystem::path unrelatedPath = sstableDir / "notes.txt";
    std::filesystem::remove_all(root);
    ASSERT_TRUE(std::filesystem::create_directories(sstableDir));
    ASSERT_NO_FATAL_FAILURE(writeFile(tempPath, "partial sstable bytes"));
    ASSERT_NO_FATAL_FAILURE(writeFile(unrelatedPath, "kept"));

    {
        const DB db(root, kManualFlushThreshold);

        EXPECT_FALSE(std::filesystem::exists(tempPath));
        EXPECT_TRUE(std::filesystem::is_regular_file(unrelatedPath));
        EXPECT_TRUE(std::filesystem::is_regular_file(root / "wal" / "wal_0.wal"));
    }

    std::filesystem::remove_all(root);
}

TEST(DBTest, NewerFlushedSSTableWinsForDuplicateKey)
{
    const std::filesystem::path root("db_tests_newer_sstable_wins");
    std::filesystem::remove_all(root);

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "key", "old"));
        ASSERT_NO_THROW(db.flush());

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "key", "new"));
        ASSERT_NO_THROW(db.flush());

        ASSERT_NO_FATAL_FAILURE(expectGet(db, "key", "new"));
    }

    std::filesystem::remove_all(root);
}
