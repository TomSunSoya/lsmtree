#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

// Include the implementation so this test translation unit can exercise the anonymous-namespace compaction helper.
#include "../src/DB.cpp"
#include "Manifest.h"
#include "SSTable.h"
#include "test_support.h"

namespace
{
using test_support::expectFileContent;
using test_support::readFile;
using test_support::ScopedPathCleanup;
using test_support::writeFile;

constexpr uint64_t kManualFlushThreshold = std::numeric_limits<uint64_t>::max();
constexpr std::string_view kWalHeader{"LWAL\x01", 5};

std::string walContent(const std::string_view records = {})
{
    std::string content(kWalHeader);
    content += records;
    return content;
}

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

void expectGet(const DB& db, const std::string& key, const uint64_t readSeq, const std::string& expected)
{
    std::string actual;
    ASSERT_TRUE(db.get(key, readSeq, actual)) << "expected key to exist at seq " << readSeq << ": " << key;
    EXPECT_EQ(expected, actual) << "unexpected value for key: " << key;
}

void expectMissing(const DB& db, const std::string& key)
{
    std::string actual = "unchanged";
    EXPECT_FALSE(db.get(key, actual)) << "expected key to be missing: " << key;
}

void expectMissing(const DB& db, const std::string& key, const uint64_t readSeq)
{
    std::string actual = "unchanged";
    EXPECT_FALSE(db.get(key, readSeq, actual)) << "expected key to be missing at seq " << readSeq << ": " << key;
}

void expectScanValues(const DB& db, const std::string_view start, const std::string_view end, const uint64_t readSeq,
                      const std::vector<std::pair<std::string, std::string>>& expected)
{
    const auto records = db.scan(start, end, readSeq);
    ASSERT_EQ(expected.size(), records.size());
    for (size_t i = 0; i < expected.size(); ++i)
    {
        EXPECT_EQ(expected[i].first, records[i].key) << "record index: " << i;
        EXPECT_EQ(Type::VALUE, records[i].type) << "record index: " << i;
        EXPECT_EQ(expected[i].second, records[i].value) << "record index: " << i;
    }
}

void expectScanValues(const DB& db, const std::string_view start, const std::string_view end,
                      const std::vector<std::pair<std::string, std::string>>& expected)
{
    expectScanValues(db, start, end, UINT64_MAX, expected);
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

void addTableAtLevel(const std::filesystem::path& root, Manifest& manifest,
                     const std::vector<std::pair<std::string, std::string>>& records, const uint32_t targetLevel,
                     const std::optional<uint64_t> recordedSize = std::nullopt, const uint64_t startSeq = 1)
{
    const auto tableNumber = manifest.allocateNumber();
    const auto walPath = root / ("fixture_" + std::to_string(tableNumber) + ".wal");
    const auto tablePath = root / "sstable" / ("sst_" + std::to_string(tableNumber) + ".sst");
    std::pair<std::string, std::string> keyRange;

    uint64_t seq = startSeq;
    {
        MemTable table(walPath.string());
        for (const auto& [key, value] : records)
            ASSERT_TRUE(table.put(key, seq++, value)) << "expected fixture put to succeed for key: " << key;

        ASSERT_NO_THROW(keyRange = SSTable::build(table, tablePath));
    }

    // Fixture tables bypass DB::nextSeq_, so keep lastSeq ahead of every crafted seq to preserve the
    // invariant that all persisted records are visible to a read at the latest sequence number.
    if (seq - 1 > manifest.lastSeq())
        manifest.setLastSeq(seq - 1);

    ASSERT_NO_THROW(manifest.addTable(tableNumber, recordedSize.value_or(std::filesystem::file_size(tablePath)),
                                      keyRange.first, keyRange.second, targetLevel));
    std::filesystem::remove(walPath);
}

void addLevelOneTable(const std::filesystem::path& root, Manifest& manifest,
                      const std::vector<std::pair<std::string, std::string>>& records)
{
    addTableAtLevel(root, manifest, records, 1);
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

void overwriteSSTableIndexSize(const std::filesystem::path& path, const uint64_t indexSize)
{
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(file.is_open());

    file.seekp(-static_cast<std::streamoff>(sizeof(indexSize)), std::ios::end);
    ASSERT_TRUE(file.write(reinterpret_cast<const char*>(&indexSize), sizeof(indexSize)));
    ASSERT_TRUE(file.good());
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

TEST(DBTest, WriteBatchAssignsConsecutiveSequencesInOperationOrder)
{
    const std::filesystem::path root("db_tests_write_batch_operation_order");
    const ScopedPathCleanup cleanup(root);
    const std::filesystem::path walPath = root / "wal" / "wal_0.wal";

    {
        DB db(root, kManualFlushThreshold);
        WriteBatch batch;
        batch.put("key", "first");
        batch.remove("key");
        batch.put("key", "last");

        ASSERT_TRUE(db.write(batch));

        ASSERT_NO_FATAL_FAILURE(expectGet(db, "key", 1, "first"));
        ASSERT_NO_FATAL_FAILURE(expectMissing(db, "key", 2));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "key", 3, "last"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "key", "last"));
    }

    ASSERT_NO_FATAL_FAILURE(
        expectFileContent(walPath, walContent("3,P,1,3,key=5,first\nD,2,3,key=0,\nP,3,3,key=4,last\n")));
}

TEST(DBTest, WriteBatchRespectsSnapshotsTakenBeforeAndAfterBatch)
{
    const std::filesystem::path root("db_tests_write_batch_snapshot_boundaries");
    const ScopedPathCleanup cleanup(root);

    DB db(root, kManualFlushThreshold);
    ASSERT_NO_FATAL_FAILURE(expectPut(db, "key", "before"));
    const Snapshot beforeBatch = db.snapshot();

    WriteBatch batch;
    batch.put("key", "in-batch");
    batch.put("added", "visible-after-batch");
    ASSERT_TRUE(db.write(batch));
    const Snapshot afterBatch = db.snapshot();

    ASSERT_NO_FATAL_FAILURE(expectPut(db, "key", "after"));

    EXPECT_EQ(1, beforeBatch.seq());
    ASSERT_NO_FATAL_FAILURE(expectGet(db, "key", beforeBatch.seq(), "before"));
    ASSERT_NO_FATAL_FAILURE(expectMissing(db, "added", beforeBatch.seq()));

    EXPECT_EQ(3, afterBatch.seq());
    ASSERT_NO_FATAL_FAILURE(expectGet(db, "key", afterBatch.seq(), "in-batch"));
    ASSERT_NO_FATAL_FAILURE(expectGet(db, "added", afterBatch.seq(), "visible-after-batch"));
    ASSERT_NO_FATAL_FAILURE(expectGet(db, "key", "after"));
}

TEST(DBTest, ClearedAndEmptyWriteBatchDoesNotConsumeSequence)
{
    const std::filesystem::path root("db_tests_write_batch_clear");
    const ScopedPathCleanup cleanup(root);
    const std::filesystem::path walPath = root / "wal" / "wal_0.wal";

    DB db(root, kManualFlushThreshold);
    WriteBatch batch;
    batch.put("discarded", "value");
    batch.remove("also-discarded");
    batch.clear();

    ASSERT_TRUE(db.write(batch));
    const Snapshot afterEmptyBatch = db.snapshot();
    EXPECT_EQ(0, afterEmptyBatch.seq());
    expectMissing(db, "discarded");
    expectMissing(db, "also-discarded");
    ASSERT_NO_FATAL_FAILURE(expectFileContent(walPath, kWalHeader));

    batch.put("kept", "value");
    ASSERT_TRUE(db.write(batch));
    ASSERT_NO_FATAL_FAILURE(expectGet(db, "kept", 1, "value"));
    ASSERT_NO_FATAL_FAILURE(expectFileContent(walPath, walContent("1,P,1,4,kept=5,value\n")));
}

TEST(DBTest, WriteBatchRetainsRecordsAcrossWritesUntilCleared)
{
    const std::filesystem::path root("db_tests_write_batch_reuse");
    const ScopedPathCleanup cleanup(root);
    const std::filesystem::path walPath = root / "wal" / "wal_0.wal";

    {
        DB db(root, kManualFlushThreshold);
        WriteBatch batch;
        batch.put("key", "value");

        ASSERT_TRUE(db.write(batch));
        ASSERT_TRUE(db.write(batch));
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(walPath, walContent("1,P,1,3,key=5,value\n1,P,2,3,key=5,value\n")));
}

TEST(DBTest, WriteBatchReopensFromWalAndContinuesSequence)
{
    const std::filesystem::path root("db_tests_write_batch_reopen");
    const ScopedPathCleanup cleanup(root);
    const std::filesystem::path walPath = root / "wal" / "wal_0.wal";

    {
        DB db(root, kManualFlushThreshold);
        WriteBatch batch;
        batch.put("alpha", "one");
        batch.remove("ghost");

        ASSERT_TRUE(db.write(batch));
    }

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectGet(db, "alpha", "one"));
        expectMissing(db, "ghost");
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "gamma", "three"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "gamma", 3, "three"));
    }

    ASSERT_NO_FATAL_FAILURE(
        expectFileContent(walPath, walContent("2,P,1,5,alpha=3,one\nD,2,5,ghost=0,\n1,P,3,5,gamma=5,three\n")));
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
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectGet(db, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "beta", "two"));
        expectMissing(db, "gamma");
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "gamma", "three"));
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(
        root / "wal" / "wal_0.wal", walContent("1,P,1,5,alpha=3,one\n1,P,2,4,beta=3,two\n1,P,3,5,gamma=5,three\n")));
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

    ASSERT_NO_FATAL_FAILURE(expectFileContent(walPath, walContent("1,P,1,5,alpha=3,one\n1,P,2,4,beta=3,two\n")));
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
    constexpr uint64_t threshold = 5 + 3 + sizeof(Type) + sizeof(uint64_t);

    {
        DB db(root, threshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "one"));

        EXPECT_FALSE(std::filesystem::exists(sstablePath));
        EXPECT_TRUE(std::filesystem::is_regular_file(walPath));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "alpha", "one"));
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(walPath, walContent("1,P,1,5,alpha=3,one\n")));
}

TEST(DBTest, PutAutoFlushesWhenMemTableSizeExceedsThreshold)
{
    const std::filesystem::path root("db_tests_auto_flush_exceeds_threshold");
    const ScopedPathCleanup cleanup(root);
    const std::filesystem::path oldWalPath = root / "wal" / "wal_0.wal";
    const std::filesystem::path newWalPath = root / "wal" / "wal_1.wal";
    const std::filesystem::path sstablePath = root / "sstable" / "sst_0.sst";
    constexpr uint64_t threshold = 1 + 2 + sizeof(Type) + sizeof(uint64_t);

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

    ASSERT_NO_FATAL_FAILURE(expectFileContent(newWalPath, walContent("1,P,3,1,d=1,4\n")));
}

TEST(DBTest, RemoveAutoFlushesWhenTombstoneSizeExceedsThreshold)
{
    const std::filesystem::path root("db_tests_remove_auto_flush_exceeds_threshold");
    const ScopedPathCleanup cleanup(root);
    const std::filesystem::path oldWalPath = root / "wal" / "wal_0.wal";
    const std::filesystem::path newWalPath = root / "wal" / "wal_1.wal";
    const std::filesystem::path sstablePath = root / "sstable" / "sst_0.sst";
    constexpr uint64_t threshold = sizeof(Type) + sizeof(uint64_t);

    {
        DB db(root, threshold);

        ASSERT_NO_FATAL_FAILURE(expectRemove(db, "k"));

        EXPECT_TRUE(std::filesystem::is_regular_file(sstablePath));
        EXPECT_FALSE(std::filesystem::exists(oldWalPath));
        EXPECT_TRUE(std::filesystem::is_regular_file(newWalPath));
        expectMissing(db, "k");
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(newWalPath, kWalHeader));
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

TEST(DBTest, FlushPersistsSSTableMetadataInManifest)
{
    const std::filesystem::path root("db_tests_flush_persists_key_range");
    const ScopedPathCleanup cleanup(root);
    const std::filesystem::path tablePath = root / "sstable" / "sst_0.sst";

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
    EXPECT_EQ(std::filesystem::file_size(tablePath), level[0].size);
    EXPECT_EQ("alpha", level[0].minKey);
    EXPECT_EQ("zulu", level[0].maxKey);
    EXPECT_EQ(3, manifest.lastSeq());
}

TEST(DBTest, ReopenContinuesSequenceFromManifestAfterFlush)
{
    const std::filesystem::path root("db_tests_reopen_sequence_from_manifest");
    const ScopedPathCleanup cleanup(root);
    const std::filesystem::path activeWalPath = root / "wal" / "wal_1.wal";

    {
        DB db(root, kManualFlushThreshold);
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "one"));
        ASSERT_NO_THROW(db.flush());
    }

    {
        DB db(root, kManualFlushThreshold);
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "beta", "two"));
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(activeWalPath, walContent("1,P,2,4,beta=3,two\n")));
}

TEST(DBTest, ReopenContinuesSequenceFromActiveWal)
{
    const std::filesystem::path root("db_tests_reopen_sequence_from_active_wal");
    const ScopedPathCleanup cleanup(root);

    {
        DB db(root, kManualFlushThreshold);
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "beta", "two"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "gamma", "three"));
    }

    {
        DB db(root, kManualFlushThreshold);
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "delta", "four"));
        ASSERT_NO_THROW(db.flush());
    }

    const Manifest manifest(root / "MANIFEST");
    EXPECT_EQ(4, manifest.lastSeq());
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

TEST(DBTest, ReusesCachedSSTableAcrossPointReads)
{
    const std::filesystem::path root("db_tests_reuses_cached_sstable");
    const ScopedPathCleanup cleanup(root);
    const std::filesystem::path tablePath = root / "sstable" / "sst_0.sst";

    DB db(root, kManualFlushThreshold);
    ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "one"));
    ASSERT_NO_FATAL_FAILURE(expectPut(db, "beta", "two"));
    ASSERT_NO_THROW(db.flush());

    ASSERT_NO_FATAL_FAILURE(expectGet(db, "alpha", "one"));

    // Make constructing a replacement SSTable unable to discover a block. A cached object keeps the metadata
    // loaded by the first point read and can still locate a different key in the immutable records region.
    ASSERT_NO_FATAL_FAILURE(overwriteSSTableIndexSize(tablePath, 0));
    ASSERT_NO_FATAL_FAILURE(expectGet(db, "beta", "two"));
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
    ASSERT_NO_FATAL_FAILURE(expectFileContent(newWalPath, walContent("1,P,2,4,beta=3,two\n")));
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
    ASSERT_NO_FATAL_FAILURE(writeFile(oldWalPath, walContent("1,P,1,5,stale=7,ignored\n")));
    ASSERT_NO_FATAL_FAILURE(writeFile(currentWalPath, walContent("1,P,2,4,beta=3,two\n")));

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

    ASSERT_NO_FATAL_FAILURE(writeFile(staleWalPath, walContent("1,P,1,5,stale=7,ignored\n")));
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

TEST(DBTest, SnapshotGetReadsValueVisibleAtSnapshot)
{
    const std::filesystem::path root("db_tests_snapshot_reads_old_value");
    const ScopedPathCleanup cleanup(root);

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "key", "old"));
        const Snapshot snapshot = db.snapshot();
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "key", "new"));

        ASSERT_NO_FATAL_FAILURE(expectGet(db, "key", snapshot.seq(), "old"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "key", "new"));
    }
}

TEST(DBTest, SnapshotTakenBeforePutDoesNotSeeNewValue)
{
    const std::filesystem::path root("db_tests_snapshot_before_put");
    const ScopedPathCleanup cleanup(root);

    {
        DB db(root, kManualFlushThreshold);

        const Snapshot snapshot = db.snapshot();
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "key", "new"));

        ASSERT_NO_FATAL_FAILURE(expectMissing(db, "key", snapshot.seq()));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "key", "new"));
    }
}

TEST(DBTest, SnapshotGetDoesNotSeeTombstoneWrittenAfterSnapshot)
{
    const std::filesystem::path root("db_tests_snapshot_tombstone_invisible");
    const ScopedPathCleanup cleanup(root);

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "key", "old"));
        const Snapshot snapshot = db.snapshot();
        ASSERT_NO_FATAL_FAILURE(expectRemove(db, "key"));

        ASSERT_NO_FATAL_FAILURE(expectGet(db, "key", snapshot.seq(), "old"));
        expectMissing(db, "key");
    }
}

TEST(DBTest, SnapshotGetFallsThroughNewerSSTableToOlderTable)
{
    const std::filesystem::path root("db_tests_snapshot_falls_through_sstables");
    const ScopedPathCleanup cleanup(root);

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "key", "old"));
        ASSERT_NO_THROW(db.flush());

        const Snapshot snapshot = db.snapshot();

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "key", "new"));
        ASSERT_NO_THROW(db.flush());

        ASSERT_NO_FATAL_FAILURE(expectGet(db, "key", snapshot.seq(), "old"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "key", "new"));
    }
}

TEST(DBTest, ActiveSnapshotSurvivesLevelZeroCompaction)
{
    const std::filesystem::path root("db_tests_active_snapshot_survives_l0_compaction");
    const ScopedPathCleanup cleanup(root);

    DB db(root, kManualFlushThreshold);
    ASSERT_NO_FATAL_FAILURE(expectPut(db, "key", "old"));
    ASSERT_NO_THROW(db.flush());
    const Snapshot snapshot = db.snapshot();

    ASSERT_NO_FATAL_FAILURE(expectPut(db, "key", "new"));
    ASSERT_NO_THROW(db.flush());
    ASSERT_NO_THROW(db.compact());

    ASSERT_NO_FATAL_FAILURE(expectGet(db, "key", snapshot.seq(), "old"));
    ASSERT_NO_FATAL_FAILURE(expectGet(db, "key", "new"));
}

TEST(DBTest, DestroyingOneDuplicateSnapshotKeepsTheOtherSnapshotActive)
{
    const std::filesystem::path root("db_tests_duplicate_snapshot_reference_count");
    const ScopedPathCleanup cleanup(root);

    DB db(root, kManualFlushThreshold);
    ASSERT_NO_FATAL_FAILURE(expectPut(db, "key", "old"));
    ASSERT_NO_THROW(db.flush());

    const Snapshot surviving = db.snapshot();
    {
        const Snapshot dropped = db.snapshot();
        ASSERT_EQ(surviving.seq(), dropped.seq());
    }

    ASSERT_NO_FATAL_FAILURE(expectPut(db, "key", "new"));
    ASSERT_NO_THROW(db.flush());
    ASSERT_NO_THROW(db.compact());

    ASSERT_NO_FATAL_FAILURE(expectGet(db, "key", surviving.seq(), "old"));
    ASSERT_NO_FATAL_FAILURE(expectGet(db, "key", "new"));
}

TEST(DBTest, DestroyingRaiiSnapshotAllowsCompactionToDropObsoleteVersion)
{
    const std::filesystem::path root("db_tests_destroyed_raii_snapshot_allows_version_gc");
    const ScopedPathCleanup cleanup(root);

    DB db(root, kManualFlushThreshold);
    ASSERT_NO_FATAL_FAILURE(expectPut(db, "key", "old"));
    ASSERT_NO_THROW(db.flush());

    {
        const Snapshot snapshot = db.snapshot();
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "key", "new"));
        ASSERT_NO_THROW(db.flush());
    }

    ASSERT_NO_THROW(db.compact());

    const Manifest manifest(root / "MANIFEST");
    ASSERT_EQ(1, manifest.level(1).size());
    SSTableIterator iterator(sstablePath(root, manifest.level(1).front().number));
    ASSERT_TRUE(iterator.valid());
    EXPECT_EQ("key", iterator.current().key);
    EXPECT_EQ("new", iterator.current().value);
    iterator.advance();
    EXPECT_FALSE(iterator.valid());
}

TEST(DBTest, RaiiSnapshotRegistersUntilEndOfScope)
{
    const std::filesystem::path root("db_tests_raii_snapshot_scope_accounting");
    const ScopedPathCleanup cleanup(root);

    DB db(root, kManualFlushThreshold);
    EXPECT_EQ(0, db.activeSnapshotCount());
    {
        const Snapshot snapshot = db.snapshot();
        EXPECT_EQ(1, db.activeSnapshotCount());
    }
    EXPECT_EQ(0, db.activeSnapshotCount());
}

TEST(DBTest, MoveConstructedSnapshotDoesNotDoubleRelease)
{
    const std::filesystem::path root("db_tests_move_constructed_snapshot_no_double_release");
    const ScopedPathCleanup cleanup(root);

    DB db(root, kManualFlushThreshold);
    // A bystander snapshot at the same seq makes a wrong release eat a live share instead of no-oping.
    const Snapshot keep = db.snapshot();
    {
        Snapshot a = db.snapshot();
        const Snapshot b = std::move(a);
        EXPECT_EQ(2, db.activeSnapshotCount());
    }
    EXPECT_EQ(1, db.activeSnapshotCount());
}

TEST(DBTest, MoveAssignedSnapshotReleasesReplacedRegistration)
{
    const std::filesystem::path root("db_tests_move_assigned_snapshot_releases_replaced");
    const ScopedPathCleanup cleanup(root);

    DB db(root, kManualFlushThreshold);
    // Same setup as the move-construct case: the bystander share must survive the assignment accounting.
    const Snapshot keep = db.snapshot();
    {
        Snapshot a = db.snapshot();
        Snapshot b = db.snapshot();
        EXPECT_EQ(3, db.activeSnapshotCount());

        a = std::move(b);
        EXPECT_EQ(2, db.activeSnapshotCount());
    }
    EXPECT_EQ(1, db.activeSnapshotCount());
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

TEST(DBTest, CompactPersistsMergedTableMetadataInManifest)
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
    EXPECT_EQ(std::filesystem::file_size(root / "sstable" / "sst_4.sst"), level[0].size);
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
            ASSERT_TRUE(orphan.put("deleted", 1, "orphan-value"));
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

TEST(DBTest, WriteCompactionOutputKeepsEveryVersionOfOneKeyInTheSameTable)
{
    const std::filesystem::path root("db_tests_compaction_output_keeps_versions_together");
    const ScopedPathCleanup cleanup(root);
    constexpr uint64_t sliceThreshold = 30;
    std::vector<Record> records{
        {"alpha", 40, Type::VALUE, "left"}, {"shared", 30, Type::VALUE, "new"}, {"shared", 20, Type::TOMBSTONE, ""},
        {"shared", 10, Type::VALUE, "old"}, {"zulu", 0, Type::VALUE, "right"},
    };

    Manifest manifest(root / "MANIFEST");
    std::vector<TableMeta> outputTables;
    ASSERT_NO_THROW(outputTables = writeCompactionOutput(manifest, root, std::span(records), sliceThreshold));

    ASSERT_EQ(2, outputTables.size());
    for (size_t left = 0; left < outputTables.size(); ++left)
    {
        for (size_t right = left + 1; right < outputTables.size(); ++right)
        {
            EXPECT_FALSE(rangesOverlap(outputTables[left], outputTables[right].minKey, outputTables[right].maxKey));
        }
    }

    std::vector<Record> persisted;
    for (const TableMeta& table : outputTables)
    {
        SSTableIterator iterator(sstablePath(root, table.number));
        std::optional<Key> previous;
        const size_t tableBegin = persisted.size();
        while (iterator.valid())
        {
            const Record& current = iterator.current();
            const Key currentKey{current.key, current.seq};
            if (previous)
                EXPECT_TRUE(*previous < currentKey);
            previous = currentKey;
            persisted.push_back(current);
            iterator.advance();
        }

        ASSERT_LT(tableBegin, persisted.size());
        EXPECT_EQ(table.minKey, persisted[tableBegin].key);
        EXPECT_EQ(table.maxKey, persisted.back().key);
    }

    ASSERT_EQ(records.size(), persisted.size());
    for (size_t index = 0; index < records.size(); ++index)
    {
        EXPECT_EQ(records[index].key, persisted[index].key) << "record index: " << index;
        EXPECT_EQ(records[index].seq, persisted[index].seq) << "record index: " << index;
        EXPECT_EQ(records[index].type, persisted[index].type) << "record index: " << index;
        EXPECT_EQ(records[index].value, persisted[index].value) << "record index: " << index;
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

TEST(DBTest, FlushCascadesLevelZeroCompactionIntoLevelTwoWithoutAnotherFlush)
{
    const std::filesystem::path root("db_tests_flush_cascades_to_l2");
    const ScopedPathCleanup cleanup(root);
    constexpr uint64_t compactThreshold = 1;
    constexpr uint64_t baseBudget = 64;

    DB db(root, kManualFlushThreshold, compactThreshold, kManualFlushThreshold, baseBudget);
    ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "value-alpha-0123456789"));
    ASSERT_NO_FATAL_FAILURE(expectPut(db, "bravo", "value-bravo-0123456789"));
    ASSERT_NO_THROW(db.flush());

    {
        const Manifest beforeCascadingFlush(root / "MANIFEST");
        ASSERT_EQ(1, beforeCascadingFlush.level(0).size());
        EXPECT_TRUE(beforeCascadingFlush.level(1).empty());
        EXPECT_TRUE(beforeCascadingFlush.level(2).empty());
    }

    ASSERT_NO_FATAL_FAILURE(expectPut(db, "charlie", "value-charlie-0123456789"));
    ASSERT_NO_FATAL_FAILURE(expectPut(db, "delta", "value-delta-0123456789"));
    ASSERT_NO_THROW(db.flush());

    {
        const Manifest afterCascadingFlush(root / "MANIFEST");
        EXPECT_TRUE(afterCascadingFlush.level(0).empty());
        EXPECT_TRUE(afterCascadingFlush.level(1).empty());
        ASSERT_EQ(1, afterCascadingFlush.level(2).size());
        EXPECT_GT(afterCascadingFlush.level(2).front().size, baseBudget);
        EXPECT_LE(afterCascadingFlush.level(2).front().size, 10 * baseBudget);
    }

    ASSERT_NO_FATAL_FAILURE(expectGet(db, "alpha", "value-alpha-0123456789"));
    ASSERT_NO_FATAL_FAILURE(expectGet(db, "bravo", "value-bravo-0123456789"));
    ASSERT_NO_FATAL_FAILURE(expectGet(db, "charlie", "value-charlie-0123456789"));
    ASSERT_NO_FATAL_FAILURE(expectGet(db, "delta", "value-delta-0123456789"));
}

TEST(DBTest, LevelOneCompactionUsesBaseByteBudgetWhenDeeperLevelsExist)
{
    const std::filesystem::path root("db_tests_l1_uses_base_budget");
    const ScopedPathCleanup cleanup(root);
    constexpr uint64_t baseBudget = 1000;

    {
        const DB db(root, kManualFlushThreshold);
    }

    uint64_t levelOneNumber = 0;
    {
        Manifest manifest(root / "MANIFEST");
        ASSERT_NO_FATAL_FAILURE(addTableAtLevel(root, manifest, {{"alpha", "one"}}, 1, baseBudget + 1));
        ASSERT_NO_FATAL_FAILURE(addTableAtLevel(root, manifest, {{"zulu", "last"}}, 2, 1));
        levelOneNumber = manifest.level(1).front().number;
        ASSERT_NO_THROW(manifest.save());
    }

    {
        DB db(root, kManualFlushThreshold, kManualFlushThreshold, kManualFlushThreshold, baseBudget);
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "trigger", "value"));
        ASSERT_NO_THROW(db.flush());
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "zulu", "last"));
    }

    const Manifest manifest(root / "MANIFEST");
    EXPECT_TRUE(manifest.level(1).empty());
    ASSERT_EQ(2, manifest.level(2).size());
    EXPECT_FALSE(std::filesystem::exists(sstablePath(root, levelOneNumber)));
}

TEST(DBTest, LevelTwoCompactionUsesTenTimesBaseBudgetWhenLevelThreeExists)
{
    const std::filesystem::path root("db_tests_l2_uses_ten_times_base_budget");
    const ScopedPathCleanup cleanup(root);
    constexpr uint64_t baseBudget = 1000;

    {
        const DB db(root, kManualFlushThreshold);
    }

    uint64_t levelTwoNumber = 0;
    {
        Manifest manifest(root / "MANIFEST");
        ASSERT_NO_FATAL_FAILURE(addTableAtLevel(root, manifest, {{"alpha", "one"}}, 2, 10 * baseBudget + 1));
        ASSERT_NO_FATAL_FAILURE(addTableAtLevel(root, manifest, {{"zulu", "last"}}, 3, 1));
        levelTwoNumber = manifest.level(2).front().number;
        ASSERT_NO_THROW(manifest.save());
    }

    {
        DB db(root, kManualFlushThreshold, kManualFlushThreshold, kManualFlushThreshold, baseBudget);
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "trigger", "value"));
        ASSERT_NO_THROW(db.flush());
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "zulu", "last"));
    }

    const Manifest manifest(root / "MANIFEST");
    EXPECT_TRUE(manifest.level(2).empty());
    ASSERT_EQ(2, manifest.level(3).size());
    EXPECT_FALSE(std::filesystem::exists(sstablePath(root, levelTwoNumber)));
}

TEST(DBTest, LevelTwoCompactionDoesNotRunAtExactlyTenTimesBaseBudget)
{
    const std::filesystem::path root("db_tests_l2_equal_budget_no_compact");
    const ScopedPathCleanup cleanup(root);
    constexpr uint64_t baseBudget = 1000;

    {
        const DB db(root, kManualFlushThreshold);
    }

    uint64_t levelTwoNumber = 0;
    {
        Manifest manifest(root / "MANIFEST");
        ASSERT_NO_FATAL_FAILURE(addTableAtLevel(root, manifest, {{"alpha", "one"}}, 2, 10 * baseBudget));
        ASSERT_NO_FATAL_FAILURE(addTableAtLevel(root, manifest, {{"zulu", "last"}}, 3, 1));
        levelTwoNumber = manifest.level(2).front().number;
        ASSERT_NO_THROW(manifest.save());
    }

    {
        DB db(root, kManualFlushThreshold, kManualFlushThreshold, kManualFlushThreshold, baseBudget);
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "trigger", "value"));
        ASSERT_NO_THROW(db.flush());
    }

    const Manifest manifest(root / "MANIFEST");
    ASSERT_EQ(1, manifest.level(2).size());
    EXPECT_EQ(levelTwoNumber, manifest.level(2).front().number);
    ASSERT_EQ(1, manifest.level(3).size());
    EXPECT_TRUE(std::filesystem::is_regular_file(sstablePath(root, levelTwoNumber)));
}

TEST(DBTest, LevelCompactionStartsAtFirstTableAndAdvancesCursorWithinOneDBInstance)
{
    const std::filesystem::path root("db_tests_level_compaction_cursor");
    const ScopedPathCleanup cleanup(root);

    {
        const DB db(root, kManualFlushThreshold);
    }

    uint64_t firstNumber = 0;
    uint64_t secondNumber = 0;
    uint64_t thirdNumber = 0;
    uint64_t baseBudget = 0;
    {
        Manifest manifest(root / "MANIFEST");
        ASSERT_NO_FATAL_FAILURE(addTableAtLevel(root, manifest, {{"aa", "value"}}, 1));
        ASSERT_NO_FATAL_FAILURE(addTableAtLevel(root, manifest, {{"gg", "value"}}, 1));
        ASSERT_NO_FATAL_FAILURE(addTableAtLevel(root, manifest, {{"mm", "value"}}, 1));

        const auto& levelOne = manifest.level(1);
        ASSERT_EQ(3, levelOne.size());
        ASSERT_EQ(levelOne[0].size, levelOne[1].size);
        ASSERT_EQ(levelOne[1].size, levelOne[2].size);
        firstNumber = levelOne[0].number;
        secondNumber = levelOne[1].number;
        thirdNumber = levelOne[2].number;
        baseBudget = levelOne[1].size + levelOne[2].size;
        ASSERT_NO_THROW(manifest.save());
    }

    const auto firstPath = sstablePath(root, firstNumber);
    const auto secondPath = sstablePath(root, secondNumber);
    const auto thirdPath = sstablePath(root, thirdNumber);

    DB db(root, kManualFlushThreshold, kManualFlushThreshold, kManualFlushThreshold, baseBudget);
    ASSERT_NO_FATAL_FAILURE(expectPut(db, "00", "value"));
    ASSERT_NO_THROW(db.flush());

    EXPECT_FALSE(std::filesystem::exists(firstPath));
    EXPECT_TRUE(std::filesystem::is_regular_file(secondPath));
    EXPECT_TRUE(std::filesystem::is_regular_file(thirdPath));
    {
        const Manifest afterFirstCompaction(root / "MANIFEST");
        ASSERT_EQ(2, afterFirstCompaction.level(1).size());
        EXPECT_EQ("gg", afterFirstCompaction.level(1)[0].minKey);
        EXPECT_EQ("mm", afterFirstCompaction.level(1)[1].minKey);
        ASSERT_EQ(1, afterFirstCompaction.level(2).size());
        EXPECT_EQ("aa", afterFirstCompaction.level(2).front().minKey);
    }

    ASSERT_NO_THROW(db.compact());
    ASSERT_NO_FATAL_FAILURE(expectPut(db, "yy", "value"));
    ASSERT_NO_THROW(db.flush());

    EXPECT_FALSE(std::filesystem::exists(secondPath));
    EXPECT_TRUE(std::filesystem::is_regular_file(thirdPath));
    {
        const Manifest afterSecondCompaction(root / "MANIFEST");
        ASSERT_EQ(2, afterSecondCompaction.level(1).size());
        EXPECT_EQ("00", afterSecondCompaction.level(1)[0].minKey);
        EXPECT_EQ("mm", afterSecondCompaction.level(1)[1].minKey);
        ASSERT_EQ(2, afterSecondCompaction.level(2).size());
        EXPECT_EQ("aa", afterSecondCompaction.level(2)[0].minKey);
        EXPECT_EQ("gg", afterSecondCompaction.level(2)[1].minKey);
    }
}

TEST(DBTest, LevelCompactionMergesOnlyNextLevelTablesThatOverlapAtInclusiveBounds)
{
    const std::filesystem::path root("db_tests_level_compaction_inclusive_overlap");
    const ScopedPathCleanup cleanup(root);
    constexpr uint64_t baseBudget = 1000;

    {
        const DB db(root, kManualFlushThreshold);
    }

    uint64_t sourceNumber = 0;
    uint64_t leftOverlappingNumber = 0;
    uint64_t rightOverlappingNumber = 0;
    uint64_t disjointNumber = 0;
    {
        Manifest manifest(root / "MANIFEST");
        // The level-1 records shadow same-keyed level-2 records, so they must carry newer sequence numbers.
        ASSERT_NO_FATAL_FAILURE(
            addTableAtLevel(root, manifest, {{"cc", "new-cc"}, {"ee", "new-ee"}}, 1, 10 * baseBudget + 1, 10));
        ASSERT_NO_FATAL_FAILURE(addTableAtLevel(root, manifest, {{"aa", "old-aa"}, {"cc", "old-cc"}}, 2, 1));
        ASSERT_NO_FATAL_FAILURE(addTableAtLevel(root, manifest, {{"ee", "old-ee"}, {"gg", "old-gg"}}, 2, 1));
        ASSERT_NO_FATAL_FAILURE(addTableAtLevel(root, manifest, {{"xx", "old-xx"}, {"zz", "old-zz"}}, 2, 1));
        sourceNumber = manifest.level(1).front().number;
        leftOverlappingNumber = manifest.level(2)[0].number;
        rightOverlappingNumber = manifest.level(2)[1].number;
        disjointNumber = manifest.level(2)[2].number;
        ASSERT_NO_THROW(manifest.save());
    }

    const auto sourcePath = sstablePath(root, sourceNumber);
    const auto leftOverlappingPath = sstablePath(root, leftOverlappingNumber);
    const auto rightOverlappingPath = sstablePath(root, rightOverlappingNumber);
    const auto disjointPath = sstablePath(root, disjointNumber);

    {
        DB db(root, kManualFlushThreshold, kManualFlushThreshold, kManualFlushThreshold, baseBudget);
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "trigger", "value"));
        ASSERT_NO_THROW(db.flush());

        EXPECT_FALSE(std::filesystem::exists(sourcePath));
        EXPECT_FALSE(std::filesystem::exists(leftOverlappingPath));
        EXPECT_FALSE(std::filesystem::exists(rightOverlappingPath));
        EXPECT_TRUE(std::filesystem::is_regular_file(disjointPath));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "aa", "old-aa"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "cc", "new-cc"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "ee", "new-ee"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "gg", "old-gg"));
        ASSERT_NO_FATAL_FAILURE(expectGet(db, "xx", "old-xx"));
    }

    const Manifest manifest(root / "MANIFEST");
    EXPECT_TRUE(manifest.level(1).empty());
    ASSERT_EQ(2, manifest.level(2).size());
    EXPECT_EQ("aa", manifest.level(2)[0].minKey);
    EXPECT_EQ("gg", manifest.level(2)[0].maxKey);
    EXPECT_EQ(disjointNumber, manifest.level(2)[1].number);
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

TEST(DBTest, ScanReturnsOnlyNewestVersionForRepeatedKeyInActiveMemTable)
{
    const std::filesystem::path root("db_tests_scan_repeated_active_key");
    const ScopedPathCleanup cleanup(root);

    {
        DB db(root, kManualFlushThreshold);

        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "old"));
        ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "new"));

        ASSERT_NO_FATAL_FAILURE(expectScanValues(db, "", "~", {{"alpha", "new"}}));
    }
}

TEST(DBTest, SnapshotScanReadsVisibleVersionsFromActiveMemTable)
{
    const std::filesystem::path root("db_tests_snapshot_scan_active_memtable");
    const ScopedPathCleanup cleanup(root);

    DB db(root, kManualFlushThreshold);
    ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "old"));
    ASSERT_NO_FATAL_FAILURE(expectPut(db, "deleted", "visible"));
    const Snapshot snapshot = db.snapshot();

    ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "new"));
    ASSERT_NO_FATAL_FAILURE(expectRemove(db, "deleted"));
    ASSERT_NO_FATAL_FAILURE(expectPut(db, "fresh", "new-value"));

    ASSERT_NO_FATAL_FAILURE(expectScanValues(db, "", "~", snapshot.seq(),
                                             {
                                                 {"alpha", "old"},
                                                 {"deleted", "visible"},
                                             }));
    ASSERT_NO_FATAL_FAILURE(expectScanValues(db, "", "~",
                                             {
                                                 {"alpha", "new"},
                                                 {"fresh", "new-value"},
                                             }));
}

TEST(DBTest, SnapshotScanIgnoresTombstoneWrittenAfterSnapshot)
{
    const std::filesystem::path root("db_tests_snapshot_scan_ignores_newer_tombstone");
    const ScopedPathCleanup cleanup(root);

    DB db(root, kManualFlushThreshold);
    ASSERT_NO_FATAL_FAILURE(expectPut(db, "k", "v"));
    const Snapshot snapshot = db.snapshot();
    ASSERT_NO_FATAL_FAILURE(expectRemove(db, "k"));

    ASSERT_NO_FATAL_FAILURE(expectScanValues(db, "", "~", snapshot.seq(), {{"k", "v"}}));
    ASSERT_NO_FATAL_FAILURE(expectScanValues(db, "", "~", {}));
}

TEST(DBTest, SnapshotScanFallsBackToOlderSSTableWhenNewerTableIsInvisible)
{
    const std::filesystem::path root("db_tests_snapshot_scan_falls_back_to_older_sstable");
    const ScopedPathCleanup cleanup(root);

    DB db(root, kManualFlushThreshold);
    ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "old"));
    ASSERT_NO_FATAL_FAILURE(expectPut(db, "deleted", "visible"));
    ASSERT_NO_THROW(db.flush());
    const Snapshot snapshot = db.snapshot();

    ASSERT_NO_FATAL_FAILURE(expectPut(db, "alpha", "new"));
    ASSERT_NO_FATAL_FAILURE(expectRemove(db, "deleted"));
    ASSERT_NO_FATAL_FAILURE(expectPut(db, "fresh", "new-value"));
    ASSERT_NO_THROW(db.flush());

    ASSERT_NO_FATAL_FAILURE(expectScanValues(db, "", "~", snapshot.seq(),
                                             {
                                                 {"alpha", "old"},
                                                 {"deleted", "visible"},
                                             }));
    ASSERT_NO_FATAL_FAILURE(expectScanValues(db, "", "~",
                                             {
                                                 {"alpha", "new"},
                                                 {"fresh", "new-value"},
                                             }));
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
