#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "DB.h"
#include "Manifest.h"
#include "SSTable.h"

namespace
{
constexpr uint64_t kManualFlushThreshold = std::numeric_limits<uint64_t>::max();

void expectPut(DB &db, const std::string &key, const std::string &value)
{
    ASSERT_TRUE(db.put(key, value)) << "expected put to succeed for key: " << key;
}

void expectRemove(DB &db, const std::string &key)
{
    ASSERT_TRUE(db.remove(key)) << "expected remove to succeed for key: " << key;
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

void expectScanValues(
    const DB &db,
    const std::string_view start,
    const std::string_view end,
    const std::vector<std::pair<std::string, std::string>> &expected)
{
    const auto records = db.scan(start, end);
    ASSERT_EQ(expected.size(), records.size());
    for (size_t i = 0; i < expected.size(); ++i)
    {
        EXPECT_EQ(expected[i].first, records[i].key) << "record index: " << i;
        EXPECT_EQ(Type::VALUE, records[i].type) << "record index: " << i;
        EXPECT_EQ(expected[i].second, records[i].value) << "record index: " << i;
    }
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

void seedCompactedLevelOne(DB &db)
{
    ASSERT_NO_FATAL_FAILURE(expectPut(db, "l1-only", "l1-value"));
    ASSERT_NO_THROW(db.flush());
    ASSERT_NO_FATAL_FAILURE(expectPut(db, "shared", "l1-shared"));
    ASSERT_NO_THROW(db.flush());
    ASSERT_NO_FATAL_FAILURE(expectPut(db, "zulu", "l1-zulu"));
    ASSERT_NO_THROW(db.flush());
}

void expectOneTableInLevelZeroAndOne(const std::filesystem::path &root)
{
    const Manifest manifest(root / "MANIFEST");
    ASSERT_EQ(1, manifest.level(0).size());
    ASSERT_EQ(1, manifest.level(1).size());
}

void addLevelOneTable(
    const std::filesystem::path &root,
    Manifest &manifest,
    const std::vector<std::pair<std::string, std::string>> &records)
{
    const auto tableNumber = manifest.allocateNumber();
    const auto walPath = root / ("fixture_" + std::to_string(tableNumber) + ".wal");
    const auto tablePath = root / "sstable" / ("sst_" + std::to_string(tableNumber) + ".sst");
    std::pair<std::string, std::string> keyRange;

    {
        MemTable table(walPath.string());
        for (const auto &[key, value] : records)
            ASSERT_TRUE(table.put(key, value)) << "expected fixture put to succeed for key: " << key;

        ASSERT_NO_THROW(keyRange = SSTable::build(table, tablePath));
    }

    ASSERT_NO_THROW(manifest.addTable(tableNumber, keyRange.first, keyRange.second, 1));
    std::filesystem::remove(walPath);
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

TEST(DBTest, ReopensFromWalWithDeletedKeyHidden)
{
    const std::filesystem::path root("db_tests_reopen_from_wal_with_delete");
    std::filesystem::remove_all(root);

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectRemove(db, "alpha"));
        expectMissing(db, "alpha");
    }

    {
        const DB db(root, kManualFlushThreshold);

        expectMissing(db, "alpha");
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

TEST(DBTest, FlushedTombstoneHidesOlderSSTableValueButKeepsOtherKeys)
{
    const std::filesystem::path root("db_tests_flushed_tombstone");
    std::filesystem::remove_all(root);

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "beta", "two"));
        ASSERT_NO_THROW(db.flush());

        ASSERT_NO_FATAL_FAILURE(expectRemove(db, "alpha"));
        ASSERT_NO_THROW(db.flush());

        expectMissing(db, "alpha");
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "beta", "two"));
    }

    {
        const DB db(root, kManualFlushThreshold);

        expectMissing(db, "alpha");
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "beta", "two"));
    }

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

TEST(DBTest, EmptyFlushDoesNotAllocateSSTableOrRotateWal)
{
    const std::filesystem::path root("db_tests_empty_flush_noop");
    const std::filesystem::path walPath = root / "wal" / "wal_0.wal";
    const std::filesystem::path nextWalPath = root / "wal" / "wal_1.wal";
    const std::filesystem::path sstablePath = root / "sstable" / "sst_0.sst";
    std::filesystem::remove_all(root);

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_THROW(db.flush());

        EXPECT_TRUE(std::filesystem::is_regular_file(walPath));
        EXPECT_FALSE(std::filesystem::exists(nextWalPath));
        EXPECT_FALSE(std::filesystem::exists(sstablePath));
    }

    const Manifest manifest(root / "MANIFEST");
    EXPECT_TRUE(manifest.allTableNumbers().empty());
    EXPECT_EQ(0, manifest.logNumber());
    EXPECT_EQ(0, manifest.nextNumber());

    std::filesystem::remove_all(root);
}

TEST(DBTest, FlushPersistsSSTableKeyRangeInManifest)
{
    const std::filesystem::path root("db_tests_flush_persists_key_range");
    std::filesystem::remove_all(root);

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "middle", "value"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "zulu", "last"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "first"));
        ASSERT_NO_THROW(db.flush());
    }

    const Manifest manifest(root / "MANIFEST");
    const auto &level = manifest.level(0);
    ASSERT_EQ(1, level.size());
    EXPECT_EQ(0, level[0].number);
    EXPECT_EQ("alpha", level[0].minKey);
    EXPECT_EQ("zulu", level[0].maxKey);

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

TEST(DBTest, ReopenContinuesGlobalFileNumberAfterExistingFiles)
{
    const std::filesystem::path root("db_tests_continue_sstable_number");
    const std::filesystem::path firstSSTablePath = root / "sstable" / "sst_0.sst";
    const std::filesystem::path secondSSTablePath = root / "sstable" / "sst_2.sst";
    const std::filesystem::path nextWalPath = root / "wal" / "wal_3.wal";
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

TEST(DBTest, ReopensFromActiveWalAfterFlush)
{
    const std::filesystem::path root("db_tests_reopen_active_wal_after_flush");
    std::filesystem::remove_all(root);

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "before-flush", "persisted-in-sstable"));
        ASSERT_NO_THROW(db.flush());
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "after-flush", "persisted-in-wal"));
    }

    {
        const DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectGet(db, "before-flush", "persisted-in-sstable"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "after-flush", "persisted-in-wal"));
    }

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

TEST(DBTest, ConstructorRemovesSSTablesMissingFromManifestButKeepsActiveTables)
{
    const std::filesystem::path root("db_tests_cleanup_orphan_sstable");
    const std::filesystem::path activeSSTablePath = root / "sstable" / "sst_0.sst";
    const std::filesystem::path orphanSSTablePath = root / "sstable" / "sst_42.sst";
    std::filesystem::remove_all(root);

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "one"));
        ASSERT_NO_THROW(db.flush());
    }

    ASSERT_TRUE(std::filesystem::is_regular_file(activeSSTablePath));
    ASSERT_NO_FATAL_FAILURE(writeFile(orphanSSTablePath, "orphan sstable bytes"));
    ASSERT_TRUE(std::filesystem::is_regular_file(orphanSSTablePath));

    {
        const DB db(root, kManualFlushThreshold);

        EXPECT_TRUE(std::filesystem::is_regular_file(activeSSTablePath));
        EXPECT_FALSE(std::filesystem::exists(orphanSSTablePath));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "alpha", "one"));
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

TEST(DBTest, CompactPersistsMergedTableAcrossReopen)
{
    const std::filesystem::path root("db_tests_compact_persists_manifest");
    const std::filesystem::path firstInput = root / "sstable" / "sst_0.sst";
    const std::filesystem::path secondInput = root / "sstable" / "sst_2.sst";
    const std::filesystem::path compactedOutput = root / "sstable" / "sst_4.sst";
    std::filesystem::remove_all(root);

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "one"));
        ASSERT_NO_THROW(db.flush());
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "beta", "two"));
        ASSERT_NO_THROW(db.flush());

        ASSERT_NO_THROW(db.compact());

        EXPECT_FALSE(std::filesystem::exists(firstInput));
        EXPECT_FALSE(std::filesystem::exists(secondInput));
        EXPECT_TRUE(std::filesystem::is_regular_file(compactedOutput));
    }

    {
        const DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectGet(db, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "beta", "two"));
        EXPECT_TRUE(std::filesystem::is_regular_file(compactedOutput));
    }

    std::filesystem::remove_all(root);
}

TEST(DBTest, CompactPersistsMergedKeyRangeInManifest)
{
    const std::filesystem::path root("db_tests_compact_persists_key_range");
    std::filesystem::remove_all(root);

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "middle", "value"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "zulu", "last"));
        ASSERT_NO_THROW(db.flush());
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "first"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "middle", "new"));
        ASSERT_NO_THROW(db.flush());

        ASSERT_NO_THROW(db.compact());
    }

    const Manifest manifest(root / "MANIFEST");
    const auto &level = manifest.level(1);
    ASSERT_EQ(1, level.size());
    EXPECT_EQ(4, level[0].number);
    EXPECT_EQ("alpha", level[0].minKey);
    EXPECT_EQ("zulu", level[0].maxKey);

    std::filesystem::remove_all(root);
}

TEST(DBTest, CompactIgnoresOrphanedSSTableThatWouldResurrectDeletedKey)
{
    const std::filesystem::path root("db_tests_compact_ignores_orphan");
    const std::filesystem::path orphanWalPath = root / "orphan.wal";
    const std::filesystem::path orphanSSTablePath = root / "sstable" / "sst_999.sst";
    std::filesystem::remove_all(root);

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "deleted", "original"));
        ASSERT_NO_THROW(db.flush());
        ASSERT_NO_FATAL_FAILURE(expectRemove(db, "deleted"));
        ASSERT_NO_THROW(db.flush());

        {
            MemTable orphan(orphanWalPath.string());
            ASSERT_TRUE(orphan.put("deleted", "orphan-value"));
            ASSERT_NO_THROW(SSTable::build(orphan, orphanSSTablePath));
        }
        ASSERT_TRUE(std::filesystem::is_regular_file(orphanSSTablePath));

        ASSERT_NO_THROW(db.compact());

        expectMissing(db, "deleted");
    }

    {
        const DB db(root, kManualFlushThreshold);

        expectMissing(db, "deleted");
        EXPECT_FALSE(std::filesystem::exists(orphanSSTablePath));
    }

    std::filesystem::remove_all(root);
}

TEST(DBTest, CompactPreservesNewestValuesAndTombstonesAcrossReopen)
{
    const std::filesystem::path root("db_tests_compact_preserves_latest_records");
    std::filesystem::remove_all(root);

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "updated", "old"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "deleted", "old"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "stable", "kept"));
        ASSERT_NO_THROW(db.flush());

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "updated", "new"));
        ASSERT_NO_FATAL_FAILURE(expectRemove(db, "deleted"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "new-key", "new-value"));
        ASSERT_NO_THROW(db.flush());

        ASSERT_NO_THROW(db.compact());

        ASSERT_NO_FATAL_FAILURE(expectGet(db, "updated", "new"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "stable", "kept"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "new-key", "new-value"));
        expectMissing(db, "deleted");
    }

    {
        const DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectGet(db, "updated", "new"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "stable", "kept"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "new-key", "new-value"));
        expectMissing(db, "deleted");
    }

    std::filesystem::remove_all(root);
}

TEST(DBTest, CompactMergesOnlyOverlappingLevelOneTables)
{
    const std::filesystem::path root("db_tests_compact_overlapping_l1_only");
    std::filesystem::remove_all(root);

    {
        const DB db(root, kManualFlushThreshold);
    }

    uint64_t leftNumber = 0;
    uint64_t overlappingNumber = 0;
    uint64_t rightNumber = 0;
    {
        Manifest manifest(root / "MANIFEST");
        ASSERT_NO_FATAL_FAILURE(addLevelOneTable(root, manifest, {{"alpha", "left"}, {"bravo", "left"}}));
        ASSERT_NO_FATAL_FAILURE(addLevelOneTable(root, manifest, {{"delta", "old"}, {"foxtrot", "old"}}));
        ASSERT_NO_FATAL_FAILURE(addLevelOneTable(root, manifest, {{"hotel", "right"}, {"juliet", "right"}}));
        ASSERT_EQ(3, manifest.level(1).size());
        leftNumber = manifest.level(1)[0].number;
        overlappingNumber = manifest.level(1)[1].number;
        rightNumber = manifest.level(1)[2].number;
        ASSERT_NO_THROW(manifest.save());
    }

    const auto leftPath = root / "sstable" / ("sst_" + std::to_string(leftNumber) + ".sst");
    const auto overlappingPath = root / "sstable" / ("sst_" + std::to_string(overlappingNumber) + ".sst");
    const auto rightPath = root / "sstable" / ("sst_" + std::to_string(rightNumber) + ".sst");
    std::string leftContent;
    std::string rightContent;
    ASSERT_NO_FATAL_FAILURE(readFile(leftPath, leftContent));
    ASSERT_NO_FATAL_FAILURE(readFile(rightPath, rightContent));

    uint64_t l0Number = 0;
    {
        DB db(root, kManualFlushThreshold);
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "echo", "new"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "foxtrot", "new"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "golf", "new"));
        ASSERT_NO_THROW(db.flush());

        const Manifest beforeCompact(root / "MANIFEST");
        ASSERT_EQ(1, beforeCompact.level(0).size());
        l0Number = beforeCompact.level(0)[0].number;

        ASSERT_NO_THROW(db.compact());

        EXPECT_FALSE(std::filesystem::exists(root / "sstable" / ("sst_" + std::to_string(l0Number) + ".sst")));
        EXPECT_FALSE(std::filesystem::exists(overlappingPath));
        ASSERT_NO_FATAL_FAILURE(expectFileContent(leftPath, leftContent));
        ASSERT_NO_FATAL_FAILURE(expectFileContent(rightPath, rightContent));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "alpha", "left"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "delta", "old"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "foxtrot", "new"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "juliet", "right"));
    }

    {
        const Manifest manifest(root / "MANIFEST");
        EXPECT_TRUE(manifest.level(0).empty());
        const auto &level1 = manifest.level(1);
        ASSERT_EQ(3, level1.size());
        EXPECT_EQ(leftNumber, level1[0].number);
        EXPECT_EQ("alpha", level1[0].minKey);
        EXPECT_EQ("bravo", level1[0].maxKey);
        EXPECT_NE(overlappingNumber, level1[1].number);
        EXPECT_EQ("delta", level1[1].minKey);
        EXPECT_EQ("golf", level1[1].maxKey);
        EXPECT_EQ(rightNumber, level1[2].number);
        EXPECT_EQ("hotel", level1[2].minKey);
        EXPECT_EQ("juliet", level1[2].maxKey);
    }

    {
        const DB db(root, kManualFlushThreshold);
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "alpha", "left"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "delta", "old"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "echo", "new"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "foxtrot", "new"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "golf", "new"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "juliet", "right"));
    }

    std::filesystem::remove_all(root);
}

TEST(DBTest, CompactIncludesLevelOneTableInsideCombinedLevelZeroGap)
{
    const std::filesystem::path root("db_tests_compact_l1_inside_l0_gap");
    std::filesystem::remove_all(root);

    {
        DB db(root, kManualFlushThreshold);
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "a", "l0-left"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "c", "l0-left"));
        ASSERT_NO_THROW(db.flush());
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "x", "l0-right"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "z", "l0-right"));
        ASSERT_NO_THROW(db.flush());
    }

    uint64_t gapTableNumber = 0;
    {
        Manifest manifest(root / "MANIFEST");
        ASSERT_EQ(2, manifest.level(0).size());
        ASSERT_NO_FATAL_FAILURE(addLevelOneTable(root, manifest, {{"e", "l1-gap"}, {"g", "l1-gap"}}));
        ASSERT_EQ(1, manifest.level(1).size());
        gapTableNumber = manifest.level(1)[0].number;
        ASSERT_NO_THROW(manifest.save());
    }

    const auto gapTablePath = root / "sstable" / ("sst_" + std::to_string(gapTableNumber) + ".sst");
    ASSERT_TRUE(std::filesystem::is_regular_file(gapTablePath));

    {
        DB db(root, kManualFlushThreshold);
        ASSERT_NO_THROW(db.compact());
        EXPECT_FALSE(std::filesystem::exists(gapTablePath));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "a", "l0-left"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "e", "l1-gap"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "g", "l1-gap"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "z", "l0-right"));
    }

    {
        const Manifest manifest(root / "MANIFEST");
        EXPECT_TRUE(manifest.level(0).empty());
        ASSERT_EQ(1, manifest.level(1).size());
        EXPECT_NE(gapTableNumber, manifest.level(1)[0].number);
        EXPECT_EQ("a", manifest.level(1)[0].minKey);
        EXPECT_EQ("z", manifest.level(1)[0].maxKey);
    }

    {
        const DB db(root, kManualFlushThreshold);
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "a", "l0-left"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "e", "l1-gap"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "g", "l1-gap"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "z", "l0-right"));
    }

    std::filesystem::remove_all(root);
}

TEST(DBTest, CompactWithoutLevelZeroDoesNotConsumeFileNumber)
{
    const std::filesystem::path root("db_tests_compact_without_l0_noop");
    constexpr uint64_t compactThreshold = 2;
    std::filesystem::remove_all(root);

    {
        DB db(root, kManualFlushThreshold, compactThreshold);
        ASSERT_NO_FATAL_FAILURE(seedCompactedLevelOne(db));

        const Manifest beforeCompact(root / "MANIFEST");
        EXPECT_TRUE(beforeCompact.level(0).empty());
        const auto expectedNextNumber = beforeCompact.nextNumber();

        ASSERT_NO_THROW(db.compact());
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "after-noop", "value"));
        ASSERT_NO_THROW(db.flush());

        const Manifest afterFlush(root / "MANIFEST");
        ASSERT_EQ(1, afterFlush.level(0).size());
        EXPECT_EQ(expectedNextNumber, afterFlush.level(0)[0].number);
        EXPECT_TRUE(std::filesystem::is_regular_file(
            root / "sstable" / ("sst_" + std::to_string(expectedNextNumber) + ".sst")));
    }

    std::filesystem::remove_all(root);
}

TEST(DBTest, FlushAutoCompactsOnlyAfterTableCountExceedsThreshold)
{
    const std::filesystem::path root("db_tests_flush_auto_compacts");
    const std::filesystem::path firstInput = root / "sstable" / "sst_0.sst";
    const std::filesystem::path secondInput = root / "sstable" / "sst_2.sst";
    const std::filesystem::path thirdInput = root / "sstable" / "sst_4.sst";
    const std::filesystem::path compactedOutput = root / "sstable" / "sst_6.sst";
    constexpr uint64_t compactThreshold = 2;
    std::filesystem::remove_all(root);

    {
        DB db(root, kManualFlushThreshold, compactThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "first", "one"));
        ASSERT_NO_THROW(db.flush());
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "second", "two"));
        ASSERT_NO_THROW(db.flush());

        EXPECT_TRUE(std::filesystem::is_regular_file(firstInput));
        EXPECT_TRUE(std::filesystem::is_regular_file(secondInput));
        EXPECT_FALSE(std::filesystem::exists(compactedOutput));

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "third", "three"));
        ASSERT_NO_THROW(db.flush());

        EXPECT_FALSE(std::filesystem::exists(firstInput));
        EXPECT_FALSE(std::filesystem::exists(secondInput));
        EXPECT_FALSE(std::filesystem::exists(thirdInput));
        EXPECT_TRUE(std::filesystem::is_regular_file(compactedOutput));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "first", "one"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "second", "two"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "third", "three"));
    }

    {
        const DB db(root, kManualFlushThreshold, compactThreshold);

        ASSERT_NO_FATAL_FAILURE(expectGet(db, "first", "one"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "second", "two"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "third", "three"));
        EXPECT_TRUE(std::filesystem::is_regular_file(compactedOutput));
    }

    std::filesystem::remove_all(root);
}

TEST(DBTest, AutoCompactMovesL0TablesToL1AndSurvivesReopen)
{
    const std::filesystem::path root("db_tests_auto_compact_l0_to_l1");
    const std::filesystem::path compactedOutput = root / "sstable" / "sst_6.sst";
    constexpr uint64_t compactThreshold = 2;
    std::filesystem::remove_all(root);

    {
        DB db(root, kManualFlushThreshold, compactThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "middle", "value"));
        ASSERT_NO_THROW(db.flush());
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "first"));
        ASSERT_NO_THROW(db.flush());
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "zulu", "last"));
        ASSERT_NO_THROW(db.flush());

        const Manifest manifest(root / "MANIFEST");
        EXPECT_TRUE(manifest.level(0).empty());
        const auto &level1 = manifest.level(1);
        ASSERT_EQ(1, level1.size());
        EXPECT_EQ(6, level1[0].number);
        EXPECT_EQ("alpha", level1[0].minKey);
        EXPECT_EQ("zulu", level1[0].maxKey);
        EXPECT_TRUE(std::filesystem::is_regular_file(compactedOutput));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "alpha", "first"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "middle", "value"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "zulu", "last"));
    }

    {
        const DB db(root, kManualFlushThreshold, compactThreshold);
        const Manifest manifest(root / "MANIFEST");
        EXPECT_TRUE(manifest.level(0).empty());
        const auto &level1 = manifest.level(1);
        ASSERT_EQ(1, level1.size());
        EXPECT_EQ(6, level1[0].number);
        EXPECT_EQ("alpha", level1[0].minKey);
        EXPECT_EQ("zulu", level1[0].maxKey);
        EXPECT_TRUE(std::filesystem::is_regular_file(compactedOutput));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "alpha", "first"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "middle", "value"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "zulu", "last"));
    }

    std::filesystem::remove_all(root);
}

TEST(DBTest, LevelZeroValueWinsOverLevelOneForSameKey)
{
    const std::filesystem::path root("db_tests_l0_value_wins_l1");
    constexpr uint64_t compactThreshold = 2;
    std::filesystem::remove_all(root);

    {
        DB db(root, kManualFlushThreshold, compactThreshold);
        ASSERT_NO_FATAL_FAILURE(seedCompactedLevelOne(db));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "shared", "l0-shared"));
        ASSERT_NO_THROW(db.flush());

        ASSERT_NO_FATAL_FAILURE(expectOneTableInLevelZeroAndOne(root));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "shared", "l0-shared"));
    }

    std::filesystem::remove_all(root);
}

TEST(DBTest, LevelZeroTombstoneHidesLevelOneValue)
{
    const std::filesystem::path root("db_tests_l0_tombstone_hides_l1");
    constexpr uint64_t compactThreshold = 2;
    std::filesystem::remove_all(root);

    {
        DB db(root, kManualFlushThreshold, compactThreshold);
        ASSERT_NO_FATAL_FAILURE(seedCompactedLevelOne(db));
        ASSERT_NO_FATAL_FAILURE(expectRemove(db, "shared"));
        ASSERT_NO_THROW(db.flush());

        ASSERT_NO_FATAL_FAILURE(expectOneTableInLevelZeroAndOne(root));
        ASSERT_NO_FATAL_FAILURE(expectMissing(db, "shared"));
    }

    std::filesystem::remove_all(root);
}

TEST(DBTest, LevelOneValueIsReadWhenLevelZeroDoesNotCoverKey)
{
    const std::filesystem::path root("db_tests_l1_read_when_l0_misses_range");
    constexpr uint64_t compactThreshold = 2;
    std::filesystem::remove_all(root);

    {
        DB db(root, kManualFlushThreshold, compactThreshold);
        ASSERT_NO_FATAL_FAILURE(seedCompactedLevelOne(db));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "zzz", "l0-other"));
        ASSERT_NO_THROW(db.flush());

        ASSERT_NO_FATAL_FAILURE(expectOneTableInLevelZeroAndOne(root));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "l1-only", "l1-value"));
    }

    std::filesystem::remove_all(root);
}

TEST(DBTest, MissingKeyReturnsFalseAcrossLevelZeroAndLevelOne)
{
    const std::filesystem::path root("db_tests_missing_across_l0_l1");
    constexpr uint64_t compactThreshold = 2;
    std::filesystem::remove_all(root);

    {
        DB db(root, kManualFlushThreshold, compactThreshold);
        ASSERT_NO_FATAL_FAILURE(seedCompactedLevelOne(db));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "zzz", "l0-other"));
        ASSERT_NO_THROW(db.flush());

        ASSERT_NO_FATAL_FAILURE(expectOneTableInLevelZeroAndOne(root));
        ASSERT_NO_FATAL_FAILURE(expectMissing(db, "missing"));
    }

    std::filesystem::remove_all(root);
}

TEST(DBTest, ScanReturnsSortedHalfOpenRangeFromActiveMemTable)
{
    const std::filesystem::path root("db_tests_scan_active_memtable");
    std::filesystem::remove_all(root);

    {
        DB db(root, kManualFlushThreshold);
        ASSERT_NO_FATAL_FAILURE(expectScanValues(db, "", "~", {}));

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "gamma", "three"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "delta", "four"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "beta", "two"));

        ASSERT_NO_FATAL_FAILURE(expectScanValues(
            db,
            "beta",
            "gamma",
            {
                {"beta", "two"},
                {"delta", "four"},
            }));
        ASSERT_NO_FATAL_FAILURE(expectScanValues(db, "gamma", "gamma", {}));
        ASSERT_NO_FATAL_FAILURE(expectScanValues(db, "z", "a", {}));
    }

    std::filesystem::remove_all(root);
}

TEST(DBTest, ScanActiveMemTableOverridesSSTableAndOmitsDeletedKeys)
{
    const std::filesystem::path root("db_tests_scan_memtable_override");
    std::filesystem::remove_all(root);

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "old"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "deleted", "old"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "stable", "kept"));
        ASSERT_NO_THROW(db.flush());

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "new"));
        ASSERT_NO_FATAL_FAILURE(expectRemove(db, "deleted"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "fresh", "value"));

        ASSERT_NO_FATAL_FAILURE(expectScanValues(
            db,
            "",
            "~",
            {
                {"alpha", "new"},
                {"fresh", "value"},
                {"stable", "kept"},
            }));
    }

    std::filesystem::remove_all(root);
}

TEST(DBTest, ScanUsesNewestSSTableAndSurvivesReopen)
{
    const std::filesystem::path root("db_tests_scan_newest_sstable");
    std::filesystem::remove_all(root);
    const std::vector<std::pair<std::string, std::string>> expected{
        {"alpha", "new"},
        {"new-key", "new-value"},
        {"stable", "kept"},
    };

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "old"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "deleted", "old"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "stable", "kept"));
        ASSERT_NO_THROW(db.flush());

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "new"));
        ASSERT_NO_FATAL_FAILURE(expectRemove(db, "deleted"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "new-key", "new-value"));
        ASSERT_NO_THROW(db.flush());

        ASSERT_NO_FATAL_FAILURE(expectScanValues(db, "", "~", expected));
    }

    {
        const DB db(root, kManualFlushThreshold);
        ASSERT_NO_FATAL_FAILURE(expectScanValues(db, "", "~", expected));
    }

    std::filesystem::remove_all(root);
}

TEST(DBTest, ScanPrefersLevelZeroRecordsAndKeepsLevelOneOnlyKeys)
{
    const std::filesystem::path root("db_tests_scan_l0_over_l1");
    constexpr uint64_t compactThreshold = 2;
    std::filesystem::remove_all(root);

    {
        DB db(root, kManualFlushThreshold, compactThreshold);
        ASSERT_NO_FATAL_FAILURE(seedCompactedLevelOne(db));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "shared", "l0-shared"));
        ASSERT_NO_FATAL_FAILURE(expectRemove(db, "zulu"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "tango", "l0-only"));
        ASSERT_NO_THROW(db.flush());

        ASSERT_NO_FATAL_FAILURE(expectOneTableInLevelZeroAndOne(root));
        ASSERT_NO_FATAL_FAILURE(expectScanValues(
            db,
            "l1-only",
            "zzzz",
            {
                {"l1-only", "l1-value"},
                {"shared", "l0-shared"},
                {"tango", "l0-only"},
            }));
    }

    std::filesystem::remove_all(root);
}

TEST(DBTest, ScanKeepsKeysFromEveryOverlappingLevelOneTable)
{
    const std::filesystem::path root("db_tests_scan_partial_l1_tables");
    std::filesystem::remove_all(root);

    {
        const DB db(root, kManualFlushThreshold);
    }

    {
        Manifest manifest(root / "MANIFEST");
        ASSERT_NO_FATAL_FAILURE(addLevelOneTable(root, manifest, {{"alpha", "one"}, {"bravo", "two"}}));
        ASSERT_NO_FATAL_FAILURE(addLevelOneTable(root, manifest, {{"delta", "four"}, {"foxtrot", "six"}}));
        ASSERT_NO_FATAL_FAILURE(addLevelOneTable(root, manifest, {{"hotel", "eight"}, {"juliet", "ten"}}));
        ASSERT_NO_THROW(manifest.save());
    }

    {
        const DB db(root, kManualFlushThreshold);
        ASSERT_NO_FATAL_FAILURE(expectScanValues(
            db,
            "charlie",
            "india",
            {
                {"delta", "four"},
                {"foxtrot", "six"},
                {"hotel", "eight"},
            }));
    }

    std::filesystem::remove_all(root);
}

TEST(DBTest, ScanIncludesStartAtTableMaxAndPrunesEndAtNextTableMin)
{
    const std::filesystem::path root("db_tests_scan_l1_touching_boundaries");
    std::filesystem::remove_all(root);

    {
        const DB db(root, kManualFlushThreshold);
    }

    uint64_t excludedTableNumber = 0;
    {
        Manifest manifest(root / "MANIFEST");
        ASSERT_NO_FATAL_FAILURE(addLevelOneTable(root, manifest, {{"alpha", "one"}, {"bravo", "two"}}));
        ASSERT_NO_FATAL_FAILURE(addLevelOneTable(root, manifest, {{"delta", "four"}, {"foxtrot", "six"}}));
        ASSERT_EQ(2, manifest.level(1).size());
        excludedTableNumber = manifest.level(1)[1].number;
        ASSERT_NO_THROW(manifest.save());
    }

    const auto excludedTablePath = root / "sstable" / ("sst_" + std::to_string(excludedTableNumber) + ".sst");
    // Make an unwanted source load observable instead of relying only on the final half-open range filter.
    ASSERT_TRUE(std::filesystem::remove(excludedTablePath));

    {
        const DB db(root, kManualFlushThreshold);
        ASSERT_NO_FATAL_FAILURE(expectScanValues(db, "bravo", "delta", {{"bravo", "two"}}));
    }

    std::filesystem::remove_all(root);
}
