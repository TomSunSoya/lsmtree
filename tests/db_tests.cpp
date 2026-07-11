#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "test_support.h"
#include "utils.h"

import lsm.db;
import lsm.manifest;
import lsm.memtable;
import lsm.sstable;

namespace
{
using test_support::expectFileContent;
using test_support::readFile;
using test_support::ScopedPathCleanup;
using test_support::writeFile;

constexpr uint64_t kManualFlushThreshold = std::numeric_limits<uint64_t>::max();

void expectPut(DB& db, const std::string& key, const std::string& value)
{
    ASSERT_TRUE(db.put(key, value)) << "expected put to succeed for key: " << key;
}

void expectRemove(DB& db, const std::string& key)
{
    ASSERT_TRUE(db.remove(key)) << "expected remove to succeed for key: " << key;
}

void expectGet(const DB& db, const std::string& key, const std::string& expected)
{
    std::string actual;
    ASSERT_TRUE(db.get(key, actual)) << "expected key to exist: " << key;
    EXPECT_EQ(expected, actual) << "unexpected value for key: " << key;
}

void expectMissing(const DB& db, const std::string& key)
{
    std::string actual = "unchanged";
    EXPECT_FALSE(db.get(key, actual)) << "expected key to be missing: " << key;
}

void expectScanValues(const DB& db, const std::string_view start, const std::string_view end,
                      const std::vector<std::pair<std::string, std::string>>& expected)
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

void seedCompactedLevelOne(DB& db)
{
    ASSERT_NO_FATAL_FAILURE(expectPut(db, "l1-only", "l1-value"));
    ASSERT_NO_THROW(db.flush());
    ASSERT_NO_FATAL_FAILURE(expectPut(db, "shared", "l1-shared"));
    ASSERT_NO_THROW(db.flush());
    ASSERT_NO_FATAL_FAILURE(expectPut(db, "zulu", "l1-zulu"));
    ASSERT_NO_THROW(db.flush());
}

void expectOneTableInLevelZeroAndOne(const std::filesystem::path& root)
{
    const Manifest manifest(root / "MANIFEST");
    ASSERT_EQ(1, manifest.level(0).size());
    ASSERT_EQ(1, manifest.level(1).size());
}

void addLevelOneTable(const std::filesystem::path& root, Manifest& manifest,
                      const std::vector<std::pair<std::string, std::string>>& records)
{
    const auto tableNumber = manifest.allocateNumber();
    const auto walPath = root / ("fixture_" + std::to_string(tableNumber) + ".wal");
    const auto tablePath = root / "sstable" / ("sst_" + std::to_string(tableNumber) + ".sst");
    std::pair<std::string, std::string> keyRange;

    {
        MemTable table(walPath.string());
        for (const auto& [key, value] : records)
            ASSERT_TRUE(table.put(key, value)) << "expected fixture put to succeed for key: " << key;

        ASSERT_NO_THROW(keyRange = SSTable::build(table, tablePath));
    }

    ASSERT_NO_THROW(manifest.addTable(tableNumber, keyRange.first, keyRange.second, 1));
    std::filesystem::remove(walPath);
}

void expectSSTableValues(const std::filesystem::path& path,
                         const std::vector<std::pair<std::string, std::string>>& expected)
{
    SSTableIterator cursor(path);
    for (size_t i = 0; i < expected.size(); ++i)
    {
        ASSERT_TRUE(cursor.valid()) << "missing record index: " << i;
        EXPECT_EQ(expected[i].first, cursor.current().key) << "record index: " << i;
        EXPECT_EQ(Type::VALUE, cursor.current().type) << "record index: " << i;
        EXPECT_EQ(expected[i].second, cursor.current().value) << "record index: " << i;
        cursor.advance();
    }
    if (cursor.valid())
        ADD_FAILURE() << "unexpected record in " << path << ": key='" << cursor.current().key << "'";
}
} // namespace

TEST(DBTest, ConstructorCreatesDataDirectoryAndWalFile)
{
    const std::filesystem::path root("db_tests_create_data_dir");
    const ScopedPathCleanup cleanup(root);

    {
        const DB db(root, kManualFlushThreshold);

        EXPECT_TRUE(std::filesystem::is_directory(root));
        EXPECT_TRUE(std::filesystem::is_directory(root / "wal"));
        EXPECT_TRUE(std::filesystem::is_regular_file(root / "wal" / "wal_0.wal"));
    }
}

TEST(DBTest, ConstructorRejectsExistingNonDirectoryPath)
{
    const std::filesystem::path root("db_tests_data_dir_is_file");
    const ScopedPathCleanup cleanup(root);
    ASSERT_NO_FATAL_FAILURE(writeFile(root, "not a directory"));

    EXPECT_THROW(DB db(root, kManualFlushThreshold), std::invalid_argument);
}

TEST(DBTest, PutAndGetUseActiveMemTable)
{
    const std::filesystem::path root("db_tests_put_get");
    const ScopedPathCleanup cleanup(root);

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "beta", "two"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "beta", "two"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "alpha", "one"));
        expectMissing(db, "gamma");
    }
}

TEST(DBTest, PutUpdatesExistingKey)
{
    const std::filesystem::path root("db_tests_update_existing");
    const ScopedPathCleanup cleanup(root);

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "key", "old"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "key", "new"));

        ASSERT_NO_FATAL_FAILURE(expectGet(db, "key", "new"));
    }
}

TEST(DBTest, PutAndGetPreserveEmptyKeysAndValues)
{
    const std::filesystem::path root("db_tests_empty_fields");
    const ScopedPathCleanup cleanup(root);

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "", "empty-key"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "empty-value", ""));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "", ""));

        ASSERT_NO_FATAL_FAILURE(expectGet(db, "", ""));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "empty-value", ""));
    }
}

TEST(DBTest, ReopensFromWal)
{
    const std::filesystem::path root("db_tests_reopen_from_wal");
    const ScopedPathCleanup cleanup(root);

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
}

TEST(DBTest, ReopensFromWalWithDeletedKeyHidden)
{
    const std::filesystem::path root("db_tests_reopen_from_wal_with_delete");
    const ScopedPathCleanup cleanup(root);

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
}

TEST(DBTest, ReopensFromWalWhenEmptySSTableDirectoryExists)
{
    const std::filesystem::path root("db_tests_reopen_wal_with_empty_sstable_dir");
    const ScopedPathCleanup cleanup(root);

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "one"));
    }

    ASSERT_TRUE(std::filesystem::create_directories(root / "sstable"));

    {
        const DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectGet(db, "alpha", "one"));
    }
}

TEST(DBTest, WritesToExpectedWalPath)
{
    const std::filesystem::path root("db_tests_wal_path");
    const ScopedPathCleanup cleanup(root);
    const std::filesystem::path walPath = root / "wal" / "wal_0.wal";

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "beta", "two"));
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(walPath, "P,5,alpha=3,one\nP,4,beta=3,two\n"));
}

TEST(DBTest, FlushedTombstoneHidesOlderSSTableValueButKeepsOtherKeys)
{
    const std::filesystem::path root("db_tests_flushed_tombstone");
    const ScopedPathCleanup cleanup(root);

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
}

TEST(DBTest, PutDoesNotAutoFlushWhenMemTableSizeEqualsThreshold)
{
    const std::filesystem::path root("db_tests_auto_flush_equal_threshold");
    const ScopedPathCleanup cleanup(root);
    const std::filesystem::path walPath = root / "wal" / "wal_0.wal";
    const std::filesystem::path sstablePath = root / "sstable" / "sst_0.sst";
    constexpr uint64_t threshold = 9;

    {
        DB db(root, threshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "one"));

        EXPECT_FALSE(std::filesystem::exists(sstablePath));
        EXPECT_TRUE(std::filesystem::is_regular_file(walPath));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "alpha", "one"));
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(walPath, "P,5,alpha=3,one\n"));
}

TEST(DBTest, PutAutoFlushesWhenMemTableSizeExceedsThreshold)
{
    const std::filesystem::path root("db_tests_auto_flush_exceeds_threshold");
    const ScopedPathCleanup cleanup(root);
    const std::filesystem::path oldWalPath = root / "wal" / "wal_0.wal";
    const std::filesystem::path newWalPath = root / "wal" / "wal_1.wal";
    const std::filesystem::path sstablePath = root / "sstable" / "sst_0.sst";
    constexpr uint64_t threshold = 6;

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
}

TEST(DBTest, FlushPublishesSSTableAndRotatesWal)
{
    const std::filesystem::path root("db_tests_flush_rotates_wal");
    const ScopedPathCleanup cleanup(root);
    const std::filesystem::path oldWalPath = root / "wal" / "wal_0.wal";
    const std::filesystem::path newWalPath = root / "wal" / "wal_1.wal";
    const std::filesystem::path sstablePath = root / "sstable" / "sst_0.sst";

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "one"));
        ASSERT_TRUE(std::filesystem::exists(oldWalPath));

        ASSERT_NO_THROW(db.flush());

        EXPECT_TRUE(std::filesystem::is_regular_file(sstablePath));
        EXPECT_FALSE(std::filesystem::exists(oldWalPath));
        EXPECT_TRUE(std::filesystem::is_regular_file(newWalPath));
    }
}

TEST(DBTest, EmptyFlushDoesNotAllocateSSTableOrRotateWal)
{
    const std::filesystem::path root("db_tests_empty_flush_noop");
    const ScopedPathCleanup cleanup(root);
    const std::filesystem::path walPath = root / "wal" / "wal_0.wal";
    const std::filesystem::path nextWalPath = root / "wal" / "wal_1.wal";
    const std::filesystem::path sstablePath = root / "sstable" / "sst_0.sst";

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
}

TEST(DBTest, FlushPersistsSSTableKeyRangeInManifest)
{
    const std::filesystem::path root("db_tests_flush_persists_key_range");
    const ScopedPathCleanup cleanup(root);

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "middle", "value"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "zulu", "last"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "first"));
        ASSERT_NO_THROW(db.flush());
    }

    const Manifest manifest(root / "MANIFEST");
    const auto& level = manifest.level(0);
    ASSERT_EQ(1, level.size());
    EXPECT_EQ(0, level[0].number);
    EXPECT_EQ("alpha", level[0].minKey);
    EXPECT_EQ("zulu", level[0].maxKey);
}

TEST(DBTest, GetFallsBackToFlushedSSTable)
{
    const std::filesystem::path root("db_tests_get_from_sstable");
    const ScopedPathCleanup cleanup(root);

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "beta", "two"));
        ASSERT_NO_THROW(db.flush());

        ASSERT_NO_FATAL_FAILURE(expectGet(db, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "beta", "two"));
        expectMissing(db, "gamma");
    }
}

TEST(DBTest, ActiveMemTableOverridesFlushedSSTable)
{
    const std::filesystem::path root("db_tests_active_overrides_sstable");
    const ScopedPathCleanup cleanup(root);

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "key", "old"));
        ASSERT_NO_THROW(db.flush());
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "key", "new"));

        ASSERT_NO_FATAL_FAILURE(expectGet(db, "key", "new"));
    }
}

TEST(DBTest, NewActiveMemTableWritesToNextWalAfterFlush)
{
    const std::filesystem::path root("db_tests_next_wal_after_flush");
    const ScopedPathCleanup cleanup(root);
    const std::filesystem::path oldWalPath = root / "wal" / "wal_0.wal";
    const std::filesystem::path newWalPath = root / "wal" / "wal_1.wal";

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "one"));
        ASSERT_NO_THROW(db.flush());
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "beta", "two"));
    }

    EXPECT_FALSE(std::filesystem::exists(oldWalPath));
    ASSERT_NO_FATAL_FAILURE(expectFileContent(newWalPath, "P,4,beta=3,two\n"));
}

TEST(DBTest, ReopensFromFlushedSSTable)
{
    const std::filesystem::path root("db_tests_reopen_from_sstable");
    const ScopedPathCleanup cleanup(root);

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
}

TEST(DBTest, ReopenContinuesGlobalFileNumberAfterExistingFiles)
{
    const std::filesystem::path root("db_tests_continue_sstable_number");
    const ScopedPathCleanup cleanup(root);
    const std::filesystem::path firstSSTablePath = root / "sstable" / "sst_0.sst";
    const std::filesystem::path secondSSTablePath = root / "sstable" / "sst_2.sst";
    const std::filesystem::path nextWalPath = root / "wal" / "wal_3.wal";

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
}

TEST(DBTest, ReopensFromActiveWalAfterFlush)
{
    const std::filesystem::path root("db_tests_reopen_active_wal_after_flush");
    const ScopedPathCleanup cleanup(root);

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
}

TEST(DBTest, ReopenRemovesWalFilesOlderThanCurrentSSTableNumber)
{
    const std::filesystem::path root("db_tests_cleanup_old_wal");
    const ScopedPathCleanup cleanup(root);
    const std::filesystem::path oldWalPath = root / "wal" / "wal_0.wal";
    const std::filesystem::path currentWalPath = root / "wal" / "wal_1.wal";

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
}

TEST(DBTest, ReopenKeepsWalFilesThatDoNotMatchOldNumberPattern)
{
    const std::filesystem::path root("db_tests_cleanup_wal_keeps_unrelated");
    const ScopedPathCleanup cleanup(root);
    const std::filesystem::path staleWalPath = root / "wal" / "wal_0.wal";
    const std::filesystem::path malformedWalPath = root / "wal" / "wal_old.wal";
    const std::filesystem::path wrongSuffixPath = root / "wal" / "wal_0.txt";
    const std::filesystem::path futureWalPath = root / "wal" / "wal_2.wal";

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
}

TEST(DBTest, ConstructorRemovesOrphanedSSTableTemporaryFiles)
{
    const std::filesystem::path root("db_tests_cleanup_sstable_tmp");
    const ScopedPathCleanup cleanup(root);
    const std::filesystem::path sstableDir = root / "sstable";
    const std::filesystem::path tempPath = sstableDir / "sst_0.sst.tmp";
    const std::filesystem::path unrelatedPath = sstableDir / "notes.txt";
    ASSERT_TRUE(std::filesystem::create_directories(sstableDir));
    ASSERT_NO_FATAL_FAILURE(writeFile(tempPath, "partial sstable bytes"));
    ASSERT_NO_FATAL_FAILURE(writeFile(unrelatedPath, "kept"));

    {
        const DB db(root, kManualFlushThreshold);

        EXPECT_FALSE(std::filesystem::exists(tempPath));
        EXPECT_TRUE(std::filesystem::is_regular_file(unrelatedPath));
        EXPECT_TRUE(std::filesystem::is_regular_file(root / "wal" / "wal_0.wal"));
    }
}

TEST(DBTest, ConstructorRemovesSSTablesMissingFromManifestButKeepsActiveTables)
{
    const std::filesystem::path root("db_tests_cleanup_orphan_sstable");
    const ScopedPathCleanup cleanup(root);
    const std::filesystem::path activeSSTablePath = root / "sstable" / "sst_0.sst";
    const std::filesystem::path orphanSSTablePath = root / "sstable" / "sst_42.sst";

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
}

TEST(DBTest, NewerFlushedSSTableWinsForDuplicateKey)
{
    const std::filesystem::path root("db_tests_newer_sstable_wins");
    const ScopedPathCleanup cleanup(root);

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "key", "old"));
        ASSERT_NO_THROW(db.flush());

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "key", "new"));
        ASSERT_NO_THROW(db.flush());

        ASSERT_NO_FATAL_FAILURE(expectGet(db, "key", "new"));
    }
}

TEST(DBTest, CompactPersistsMergedTableAcrossReopen)
{
    const std::filesystem::path root("db_tests_compact_persists_manifest");
    const ScopedPathCleanup cleanup(root);
    const std::filesystem::path firstInput = root / "sstable" / "sst_0.sst";
    const std::filesystem::path secondInput = root / "sstable" / "sst_2.sst";
    const std::filesystem::path compactedOutput = root / "sstable" / "sst_4.sst";

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
}

TEST(DBTest, CompactPersistsMergedKeyRangeInManifest)
{
    const std::filesystem::path root("db_tests_compact_persists_key_range");
    const ScopedPathCleanup cleanup(root);

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
    const auto& level = manifest.level(1);
    ASSERT_EQ(1, level.size());
    EXPECT_EQ(4, level[0].number);
    EXPECT_EQ("alpha", level[0].minKey);
    EXPECT_EQ("zulu", level[0].maxKey);
}

TEST(DBTest, CompactIgnoresOrphanedSSTableThatWouldResurrectDeletedKey)
{
    const std::filesystem::path root("db_tests_compact_ignores_orphan");
    const ScopedPathCleanup cleanup(root);
    const std::filesystem::path orphanWalPath = root / "orphan.wal";
    const std::filesystem::path orphanSSTablePath = root / "sstable" / "sst_999.sst";

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
}

TEST(DBTest, CompactPreservesNewestValuesAndTombstonesAcrossReopen)
{
    const std::filesystem::path root("db_tests_compact_preserves_latest_records");
    const ScopedPathCleanup cleanup(root);

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
}

TEST(DBTest, CompactMergesOnlyOverlappingLevelOneTables)
{
    const std::filesystem::path root("db_tests_compact_overlapping_l1_only");
    const ScopedPathCleanup cleanup(root);

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
        const auto& level1 = manifest.level(1);
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
}

TEST(DBTest, CompactIncludesLevelOneTableInsideCombinedLevelZeroGap)
{
    const std::filesystem::path root("db_tests_compact_l1_inside_l0_gap");
    const ScopedPathCleanup cleanup(root);

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
}

TEST(DBTest, CompactWithoutLevelZeroDoesNotConsumeFileNumber)
{
    const std::filesystem::path root("db_tests_compact_without_l0_noop");
    const ScopedPathCleanup cleanup(root);
    constexpr uint64_t compactThreshold = 2;

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
        EXPECT_TRUE(std::filesystem::is_regular_file(root / "sstable" /
                                                     ("sst_" + std::to_string(expectedNextNumber) + ".sst")));
    }
}

TEST(DBTest, CompactSplitsOutputIntoNonOverlappingLevelOneTablesAndSurvivesReopen)
{
    const std::filesystem::path root("db_tests_compact_splits_l1_output");
    const ScopedPathCleanup cleanup(root);
    constexpr uint64_t sliceThreshold = 20;

    const std::vector<std::pair<std::string, std::string>> expected{
        {"a", "1"}, {"b", "2"}, {"c", "3"}, {"d", "4"}, {"e", "5"},
    };

    {
        DB db(root, kManualFlushThreshold, 4, sliceThreshold);
        for (const auto& [key, value] : expected)
            ASSERT_NO_FATAL_FAILURE(expectPut(db, key, value));
        ASSERT_NO_THROW(db.flush());

        ASSERT_NO_THROW(db.compact());
        ASSERT_NO_FATAL_FAILURE(expectScanValues(db, "a", "f", expected));
        for (const auto& [key, value] : expected)
            ASSERT_NO_FATAL_FAILURE(expectGet(db, key, value));
    }

    {
        const Manifest manifest(root / "MANIFEST");
        EXPECT_TRUE(manifest.level(0).empty());
        const auto& level1 = manifest.level(1);
        ASSERT_EQ(3, level1.size());
        EXPECT_EQ("a", level1[0].minKey);
        EXPECT_EQ("b", level1[0].maxKey);
        EXPECT_EQ("c", level1[1].minKey);
        EXPECT_EQ("d", level1[1].maxKey);
        EXPECT_EQ("e", level1[2].minKey);
        EXPECT_EQ("e", level1[2].maxKey);
        for (const auto& table : level1)
            EXPECT_TRUE(
                std::filesystem::is_regular_file(root / "sstable" / ("sst_" + std::to_string(table.number) + ".sst")));

        ASSERT_NO_FATAL_FAILURE(expectSSTableValues(
            root / "sstable" / ("sst_" + std::to_string(level1[0].number) + ".sst"), {expected[0], expected[1]}));
        ASSERT_NO_FATAL_FAILURE(expectSSTableValues(
            root / "sstable" / ("sst_" + std::to_string(level1[1].number) + ".sst"), {expected[2], expected[3]}));
        ASSERT_NO_FATAL_FAILURE(expectSSTableValues(
            root / "sstable" / ("sst_" + std::to_string(level1[2].number) + ".sst"), {expected[4]}));
    }

    {
        const DB db(root, kManualFlushThreshold, 4, sliceThreshold);
        ASSERT_NO_FATAL_FAILURE(expectScanValues(db, "a", "f", expected));
        for (const auto& [key, value] : expected)
            ASSERT_NO_FATAL_FAILURE(expectGet(db, key, value));
    }
}

TEST(DBTest, FlushAutoCompactsOnlyAfterTableCountExceedsThreshold)
{
    const std::filesystem::path root("db_tests_flush_auto_compacts");
    const ScopedPathCleanup cleanup(root);
    const std::filesystem::path firstInput = root / "sstable" / "sst_0.sst";
    const std::filesystem::path secondInput = root / "sstable" / "sst_2.sst";
    const std::filesystem::path thirdInput = root / "sstable" / "sst_4.sst";
    const std::filesystem::path compactedOutput = root / "sstable" / "sst_6.sst";
    constexpr uint64_t compactThreshold = 2;

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
}

TEST(DBTest, AutoCompactMovesL0TablesToL1AndSurvivesReopen)
{
    const std::filesystem::path root("db_tests_auto_compact_l0_to_l1");
    const ScopedPathCleanup cleanup(root);
    const std::filesystem::path compactedOutput = root / "sstable" / "sst_6.sst";
    constexpr uint64_t compactThreshold = 2;

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
        const auto& level1 = manifest.level(1);
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
        const auto& level1 = manifest.level(1);
        ASSERT_EQ(1, level1.size());
        EXPECT_EQ(6, level1[0].number);
        EXPECT_EQ("alpha", level1[0].minKey);
        EXPECT_EQ("zulu", level1[0].maxKey);
        EXPECT_TRUE(std::filesystem::is_regular_file(compactedOutput));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "alpha", "first"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "middle", "value"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "zulu", "last"));
    }
}

TEST(DBTest, LevelZeroValueWinsOverLevelOneForSameKey)
{
    const std::filesystem::path root("db_tests_l0_value_wins_l1");
    const ScopedPathCleanup cleanup(root);
    constexpr uint64_t compactThreshold = 2;

    {
        DB db(root, kManualFlushThreshold, compactThreshold);
        ASSERT_NO_FATAL_FAILURE(seedCompactedLevelOne(db));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "shared", "l0-shared"));
        ASSERT_NO_THROW(db.flush());

        ASSERT_NO_FATAL_FAILURE(expectOneTableInLevelZeroAndOne(root));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "shared", "l0-shared"));
    }
}

TEST(DBTest, LevelZeroTombstoneHidesLevelOneValue)
{
    const std::filesystem::path root("db_tests_l0_tombstone_hides_l1");
    const ScopedPathCleanup cleanup(root);
    constexpr uint64_t compactThreshold = 2;

    {
        DB db(root, kManualFlushThreshold, compactThreshold);
        ASSERT_NO_FATAL_FAILURE(seedCompactedLevelOne(db));
        ASSERT_NO_FATAL_FAILURE(expectRemove(db, "shared"));
        ASSERT_NO_THROW(db.flush());

        ASSERT_NO_FATAL_FAILURE(expectOneTableInLevelZeroAndOne(root));
        ASSERT_NO_FATAL_FAILURE(expectMissing(db, "shared"));
    }
}

TEST(DBTest, LevelOneValueIsReadWhenLevelZeroDoesNotCoverKey)
{
    const std::filesystem::path root("db_tests_l1_read_when_l0_misses_range");
    const ScopedPathCleanup cleanup(root);
    constexpr uint64_t compactThreshold = 2;

    {
        DB db(root, kManualFlushThreshold, compactThreshold);
        ASSERT_NO_FATAL_FAILURE(seedCompactedLevelOne(db));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "zzz", "l0-other"));
        ASSERT_NO_THROW(db.flush());

        ASSERT_NO_FATAL_FAILURE(expectOneTableInLevelZeroAndOne(root));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "l1-only", "l1-value"));
    }
}

TEST(DBTest, MissingKeyReturnsFalseAcrossLevelZeroAndLevelOne)
{
    const std::filesystem::path root("db_tests_missing_across_l0_l1");
    const ScopedPathCleanup cleanup(root);
    constexpr uint64_t compactThreshold = 2;

    {
        DB db(root, kManualFlushThreshold, compactThreshold);
        ASSERT_NO_FATAL_FAILURE(seedCompactedLevelOne(db));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "zzz", "l0-other"));
        ASSERT_NO_THROW(db.flush());

        ASSERT_NO_FATAL_FAILURE(expectOneTableInLevelZeroAndOne(root));
        ASSERT_NO_FATAL_FAILURE(expectMissing(db, "missing"));
    }
}

TEST(DBTest, ScanReturnsSortedHalfOpenRangeFromActiveMemTable)
{
    const std::filesystem::path root("db_tests_scan_active_memtable");
    const ScopedPathCleanup cleanup(root);

    {
        DB db(root, kManualFlushThreshold);
        ASSERT_NO_FATAL_FAILURE(expectScanValues(db, "", "~", {}));

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "gamma", "three"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "delta", "four"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "beta", "two"));

        ASSERT_NO_FATAL_FAILURE(expectScanValues(db, "beta", "gamma",
                                                 {
                                                     {"beta", "two"},
                                                     {"delta", "four"},
                                                 }));
        ASSERT_NO_FATAL_FAILURE(expectScanValues(db, "gamma", "gamma", {}));
        ASSERT_NO_FATAL_FAILURE(expectScanValues(db, "z", "a", {}));
    }
}

TEST(DBTest, ScanActiveMemTableOverridesSSTableAndOmitsDeletedKeys)
{
    const std::filesystem::path root("db_tests_scan_memtable_override");
    const ScopedPathCleanup cleanup(root);

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "old"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "deleted", "old"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "stable", "kept"));
        ASSERT_NO_THROW(db.flush());

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "new"));
        ASSERT_NO_FATAL_FAILURE(expectRemove(db, "deleted"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "fresh", "value"));

        ASSERT_NO_FATAL_FAILURE(expectScanValues(db, "", "~",
                                                 {
                                                     {"alpha", "new"},
                                                     {"fresh", "value"},
                                                     {"stable", "kept"},
                                                 }));
    }
}

TEST(DBTest, ScanUsesNewestSSTableAndSurvivesReopen)
{
    const std::filesystem::path root("db_tests_scan_newest_sstable");
    const ScopedPathCleanup cleanup(root);
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
}

TEST(DBTest, ScanPrefersLevelZeroRecordsAndKeepsLevelOneOnlyKeys)
{
    const std::filesystem::path root("db_tests_scan_l0_over_l1");
    const ScopedPathCleanup cleanup(root);
    constexpr uint64_t compactThreshold = 2;

    {
        DB db(root, kManualFlushThreshold, compactThreshold);
        ASSERT_NO_FATAL_FAILURE(seedCompactedLevelOne(db));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "shared", "l0-shared"));
        ASSERT_NO_FATAL_FAILURE(expectRemove(db, "zulu"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "tango", "l0-only"));
        ASSERT_NO_THROW(db.flush());

        ASSERT_NO_FATAL_FAILURE(expectOneTableInLevelZeroAndOne(root));
        ASSERT_NO_FATAL_FAILURE(expectScanValues(db, "l1-only", "zzzz",
                                                 {
                                                     {"l1-only", "l1-value"},
                                                     {"shared", "l0-shared"},
                                                     {"tango", "l0-only"},
                                                 }));
    }
}

TEST(DBTest, ScanKeepsKeysFromEveryOverlappingLevelOneTable)
{
    const std::filesystem::path root("db_tests_scan_partial_l1_tables");
    const ScopedPathCleanup cleanup(root);

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
        ASSERT_NO_FATAL_FAILURE(expectScanValues(db, "charlie", "india",
                                                 {
                                                     {"delta", "four"},
                                                     {"foxtrot", "six"},
                                                     {"hotel", "eight"},
                                                 }));
    }
}

TEST(DBTest, ScanIncludesStartAtTableMaxAndPrunesEndAtNextTableMin)
{
    const std::filesystem::path root("db_tests_scan_l1_touching_boundaries");
    const ScopedPathCleanup cleanup(root);

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
}
