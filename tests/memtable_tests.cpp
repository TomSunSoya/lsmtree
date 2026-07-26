#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "MemTable.h"
#include "test_support.h"

namespace
{
using test_support::expectFileContent;
using test_support::kWalFrameMagic;
using test_support::kWalHeader;
using test_support::ScopedPathCleanup;
using test_support::walContent;
using test_support::walFrame;
using test_support::writeFile;

size_t entrySize(const std::string_view key, const std::string_view value)
{
    return key.size() + value.size() + sizeof(uint8_t) + sizeof(uint64_t);
}

constexpr uint64_t kLatestSequence = std::numeric_limits<uint64_t>::max();

void expectGet(const MemTable& table, const std::string& key, const std::string& expected,
               const uint64_t readSeq = kLatestSequence)
{
    std::string actual;
    ASSERT_EQ(Result::VALUE, table.get(key, readSeq, actual)) << "expected key to exist: " << key;
    EXPECT_EQ(expected, actual) << "unexpected value for key: " << key;
}

void expectPut(MemTable& table, const std::string& key, const std::string& value, const uint64_t seq = 0)
{
    ASSERT_TRUE(table.put(key, seq, value)) << "expected put to succeed for key: " << key;
}

void expectRemove(MemTable& table, const std::string& key, const uint64_t seq = 0)
{
    ASSERT_TRUE(table.remove(key, seq)) << "expected remove to succeed for key: " << key;
}

void expectMissing(const MemTable& table, const std::string& key, const uint64_t readSeq = kLatestSequence)
{
    std::string actual;
    EXPECT_EQ(Result::ABSENT, table.get(key, readSeq, actual)) << "expected key to be absent: " << key;
}

void expectTombstone(const MemTable& table, const std::string& key, const uint64_t readSeq = kLatestSequence)
{
    std::string actual = "unchanged";
    EXPECT_EQ(Result::TOMBSTONE, table.get(key, readSeq, actual)) << "expected tombstone for key: " << key;
}

void expectIteratorRecord(const Iterator& iterator, const std::string& key, const uint64_t seq, const Type type,
                          const std::string& value)
{
    ASSERT_TRUE(iterator.valid());
    EXPECT_EQ(key, iterator.current().key);
    EXPECT_EQ(seq, iterator.current().seq);
    EXPECT_EQ(type, iterator.current().type);
    EXPECT_EQ(value, iterator.current().value);
}

} // namespace

TEST(MemTableTest, ReadWrite)
{
    const std::filesystem::path logPath("memtable_tests_read_write.wal");
    const ScopedPathCleanup cleanup(logPath);

    {
        MemTable table(logPath.string());

        expectMissing(table, "missing");

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "k", "12", 1));
        ASSERT_NO_FATAL_FAILURE(expectGet(table, "k", "12"));

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "k", "123", 2));
        ASSERT_NO_FATAL_FAILURE(expectGet(table, "k", "123"));

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "", "empty-key"));
        ASSERT_NO_FATAL_FAILURE(expectGet(table, "", "empty-key"));

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "with spaces", "value with spaces"));
        ASSERT_NO_FATAL_FAILURE(expectGet(table, "with spaces", "value with spaces"));

        for (int i = 0; i < 10; ++i)
        {
            ASSERT_NO_FATAL_FAILURE(expectPut(table, "key-" + std::to_string(i), "value-" + std::to_string(i)));
        }

        for (int i = 0; i < 10; ++i)
        {
            ASSERT_NO_FATAL_FAILURE(expectGet(table, "key-" + std::to_string(i), "value-" + std::to_string(i)));
        }

        expectMissing(table, "key-10");
    }
}

TEST(MemTableTest, GetReturnsNewestVisibleVersionForSameKey)
{
    const std::filesystem::path logPath("memtable_tests_get_newest_version.wal");
    const ScopedPathCleanup cleanup(logPath);

    MemTable table(logPath.string());
    ASSERT_NO_FATAL_FAILURE(expectPut(table, "key", "old", 10));
    ASSERT_NO_FATAL_FAILURE(expectPut(table, "key", "newest", 30));
    ASSERT_NO_FATAL_FAILURE(expectPut(table, "key", "middle", 20));

    ASSERT_NO_FATAL_FAILURE(expectGet(table, "key", "newest"));
    ASSERT_NO_FATAL_FAILURE(expectGet(table, "key", "middle", 25));
    ASSERT_NO_FATAL_FAILURE(expectGet(table, "key", "old", 10));
}

TEST(MemTableTest, GetReturnsAbsentWhenMissingKeyLowerBoundLandsOnNextKey)
{
    const std::filesystem::path logPath("memtable_tests_get_missing_between_keys.wal");
    const ScopedPathCleanup cleanup(logPath);

    MemTable table(logPath.string());
    ASSERT_NO_FATAL_FAILURE(expectPut(table, "alpha", "one", 10));
    ASSERT_NO_FATAL_FAILURE(expectPut(table, "charlie", "three", 20));

    ASSERT_NO_FATAL_FAILURE(expectMissing(table, "bravo"));
}

TEST(MemTableTest, GetReturnsAbsentWhenAllVersionsAreNewerAndLowerBoundLandsOnNextKey)
{
    const std::filesystem::path logPath("memtable_tests_get_before_oldest_version.wal");
    const ScopedPathCleanup cleanup(logPath);

    MemTable table(logPath.string());
    ASSERT_NO_FATAL_FAILURE(expectPut(table, "alpha", "future", 20));
    ASSERT_NO_FATAL_FAILURE(expectPut(table, "bravo", "next-key", 1));

    ASSERT_NO_FATAL_FAILURE(expectMissing(table, "alpha", 19));
}

TEST(MemTableTest, SizeBytesStartsAtZero)
{
    const std::filesystem::path logPath("memtable_tests_size_empty.wal");
    const ScopedPathCleanup cleanup(logPath);

    {
        const MemTable table(logPath.string());

        EXPECT_EQ(0u, table.size_bytes());
    }
}

TEST(MemTableTest, SizeBytesTracksInsertedKeysAndValues)
{
    const std::filesystem::path logPath("memtable_tests_size_insert.wal");
    const ScopedPathCleanup cleanup(logPath);

    {
        MemTable table(logPath.string());

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "alpha", "one"));
        EXPECT_EQ(entrySize("alpha", "one"), table.size_bytes());

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "beta", "two"));
        EXPECT_EQ(entrySize("alpha", "one") + entrySize("beta", "two"), table.size_bytes());
    }
}

TEST(MemTableTest, SizeBytesCountsEveryVersionForSameKey)
{
    const std::filesystem::path logPath("memtable_tests_size_update.wal");
    const ScopedPathCleanup cleanup(logPath);

    {
        MemTable table(logPath.string());

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "key", "old", 1));
        ASSERT_NO_FATAL_FAILURE(expectPut(table, "key", "new-value", 2));

        EXPECT_EQ(entrySize("key", "old") + entrySize("key", "new-value"), table.size_bytes());
    }
}

TEST(MemTableTest, SizeBytesHandlesEmptyKeysAndValues)
{
    const std::filesystem::path logPath("memtable_tests_size_empty_fields.wal");
    const ScopedPathCleanup cleanup(logPath);

    {
        MemTable table(logPath.string());

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "", "value", 1));
        EXPECT_EQ(entrySize("", "value"), table.size_bytes());

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "empty-value", "", 2));
        EXPECT_EQ(entrySize("", "value") + entrySize("empty-value", ""), table.size_bytes());

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "", "", 3));
        EXPECT_EQ(entrySize("", "value") + entrySize("empty-value", "") + entrySize("", ""), table.size_bytes());
    }
}

TEST(MemTableTest, RemoveHidesExistingKeyAndWritesTombstone)
{
    const std::filesystem::path logPath("memtable_tests_remove_tombstone.wal");
    const ScopedPathCleanup cleanup(logPath);

    {
        MemTable table(logPath.string());

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "alpha", "one", 10));
        ASSERT_NO_FATAL_FAILURE(expectGet(table, "alpha", "one"));

        ASSERT_NO_FATAL_FAILURE(expectRemove(table, "alpha", 11));
        expectTombstone(table, "alpha");
        ASSERT_NO_FATAL_FAILURE(expectGet(table, "alpha", "one", 10));
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, walContent({"1,P,10,5,alpha=3,one\n", "1,D,11,5,alpha=0,\n"})));
}

TEST(MemTableTest, RemoveMissingKeyRecordsTombstoneAndReplaysAsMissing)
{
    const std::filesystem::path logPath("memtable_tests_remove_missing_tombstone.wal");
    const ScopedPathCleanup cleanup(logPath);

    {
        MemTable table(logPath.string());

        expectMissing(table, "ghost");
        ASSERT_NO_FATAL_FAILURE(expectRemove(table, "ghost", 7));
        expectTombstone(table, "ghost");
        EXPECT_EQ(entrySize("ghost", ""), table.size_bytes());
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, walContent("1,D,7,5,ghost=0,\n")));

    {
        MemTable table(logPath.string());

        expectTombstone(table, "ghost");
    }
}

TEST(MemTableTest, PutAfterRemoveRestoresKeyWithNewValue)
{
    const std::filesystem::path logPath("memtable_tests_put_after_remove.wal");
    const ScopedPathCleanup cleanup(logPath);

    {
        MemTable table(logPath.string());

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "key", "old", 20));
        ASSERT_NO_FATAL_FAILURE(expectRemove(table, "key", 21));
        expectTombstone(table, "key");

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "key", "new", 22));
        ASSERT_NO_FATAL_FAILURE(expectGet(table, "key", "new"));
        EXPECT_EQ(entrySize("key", "old") + entrySize("key", "") + entrySize("key", "new"), table.size_bytes());
    }

    ASSERT_NO_FATAL_FAILURE(
        expectFileContent(logPath, walContent({"1,P,20,3,key=3,old\n", "1,D,21,3,key=0,\n", "1,P,22,3,key=3,new\n"})));
}

TEST(MemTableTest, SizeBytesTracksTombstoneAfterRemove)
{
    const std::filesystem::path logPath("memtable_tests_size_after_remove.wal");
    const ScopedPathCleanup cleanup(logPath);

    {
        MemTable table(logPath.string());

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "alpha", "one", 1));
        EXPECT_EQ(entrySize("alpha", "one"), table.size_bytes());

        ASSERT_NO_FATAL_FAILURE(expectRemove(table, "alpha", 2));
        EXPECT_EQ(entrySize("alpha", "one") + entrySize("alpha", ""), table.size_bytes());
    }
}

TEST(MemTableTest, ApplyEmptyBatchDoesNotChangeTableOrWal)
{
    const std::filesystem::path logPath("memtable_tests_apply_empty_batch.wal");
    const ScopedPathCleanup cleanup(logPath);

    {
        MemTable table(logPath.string());

        EXPECT_TRUE(table.applyBatch({}));
        EXPECT_EQ(0u, table.size());
        EXPECT_EQ(0u, table.size_bytes());
        ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, kWalHeader));
    }

    {
        const MemTable table(logPath.string());

        EXPECT_EQ(0u, table.size());
        EXPECT_EQ(0u, table.size_bytes());
    }
}

TEST(MemTableTest, ApplyBatchWritesOneFrameAndRestoresEveryOperation)
{
    const std::filesystem::path logPath("memtable_tests_apply_batch.wal");
    const ScopedPathCleanup cleanup(logPath);
    const std::vector<Record> operations{
        {"alpha", 10, Type::VALUE, "one"},
        {"key", 20, Type::VALUE, "old"},
        {"key", 30, Type::VALUE, "new"},
        {"ghost", 40, Type::TOMBSTONE, ""},
    };
    const std::string expected =
        walContent("4,P,10,5,alpha=3,one\nP,20,3,key=3,old\nP,30,3,key=3,new\nD,40,5,ghost=0,\n");

    {
        MemTable table(logPath.string());

        ASSERT_TRUE(table.applyBatch(operations));
        EXPECT_EQ(4u, table.size());
        EXPECT_EQ(entrySize("alpha", "one") + entrySize("key", "old") + entrySize("key", "new") +
                      entrySize("ghost", ""),
                  table.size_bytes());
        ASSERT_NO_FATAL_FAILURE(expectGet(table, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectGet(table, "key", "new"));
        ASSERT_NO_FATAL_FAILURE(expectGet(table, "key", "old", 20));
        expectTombstone(table, "ghost");
        ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, expected));
    }

    {
        const MemTable table(logPath.string());

        ASSERT_NO_FATAL_FAILURE(expectGet(table, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectGet(table, "key", "new"));
        ASSERT_NO_FATAL_FAILURE(expectGet(table, "key", "old", 20));
        expectTombstone(table, "ghost");
        EXPECT_EQ(40, table.getMaxWALSeq());
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, expected));
}

TEST(MemTableTest, WALNewFileContainsHeaderAndReopensWithoutDuplicatingIt)
{
    const std::filesystem::path logPath("memtable_tests_wal_header.wal");
    const ScopedPathCleanup cleanup(logPath);

    {
        const MemTable table(logPath.string());

        EXPECT_EQ(0u, table.size());
        ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, kWalHeader));
    }

    {
        const MemTable table(logPath.string());

        EXPECT_EQ(0u, table.size());
        ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, kWalHeader));
    }
}

TEST(MemTableTest, WALRejectsTruncatedHeader)
{
    const std::filesystem::path logPath("memtable_tests_wal_truncated_header.wal");
    const ScopedPathCleanup cleanup(logPath);
    ASSERT_NO_FATAL_FAILURE(writeFile(logPath, "LWAL"));

    EXPECT_THROW({ const MemTable table(logPath.string()); }, std::runtime_error);
}

TEST(MemTableTest, WALRejectsWrongMagic)
{
    const std::filesystem::path logPath("memtable_tests_wal_wrong_magic.wal");
    const ScopedPathCleanup cleanup(logPath);
    std::string content = walContent("1,P,1,1,k=1,v\n");
    content[0] = 'X';
    ASSERT_NO_FATAL_FAILURE(writeFile(logPath, content));

    EXPECT_THROW({ const MemTable table(logPath.string()); }, std::runtime_error);
}

TEST(MemTableTest, WALRejectsUnsupportedVersion)
{
    const std::filesystem::path logPath("memtable_tests_wal_unsupported_version.wal");
    const ScopedPathCleanup cleanup(logPath);
    std::string content = walContent("1,P,1,1,k=1,v\n");
    content[4] = '\x01';
    ASSERT_NO_FATAL_FAILURE(writeFile(logPath, content));

    EXPECT_THROW({ const MemTable table(logPath.string()); }, std::runtime_error);
}

TEST(MemTableTest, WALRestoresExistingRecords)
{
    const std::filesystem::path logPath("memtable_tests_restore_existing.wal");
    const ScopedPathCleanup cleanup(logPath);
    const std::string content = walContent({"1,P,12,5,alpha=3,one\n", "1,P,47,4,beta=3,two\n"});
    ASSERT_NO_FATAL_FAILURE(writeFile(logPath, content));

    {
        MemTable table(logPath.string());

        ASSERT_NO_FATAL_FAILURE(expectGet(table, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectGet(table, "beta", "two"));
        expectMissing(table, "missing");
        EXPECT_EQ(47, table.getMaxWALSeq());
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, content));
}

TEST(MemTableTest, WALReplayRestoresMemTableSizeBytes)
{
    const std::filesystem::path logPath("memtable_tests_restore_size_bytes.wal");
    const ScopedPathCleanup cleanup(logPath);
    const std::string content = walContent({"1,P,12,5,alpha=3,one\n", "1,D,47,4,beta=0,\n"});
    ASSERT_NO_FATAL_FAILURE(writeFile(logPath, content));

    const MemTable table(logPath.string());

    EXPECT_EQ(entrySize("alpha", "one") + entrySize("beta", ""), table.size_bytes())
        << "replayed records must participate in the same flush threshold accounting as new writes";
}

TEST(MemTableTest, WALRestoresEveryRecordFromCompleteMultiRecordBatch)
{
    const std::filesystem::path logPath("memtable_tests_restore_complete_batch.wal");
    const ScopedPathCleanup cleanup(logPath);
    const std::string content = walContent("3,P,10,5,alpha=3,one\nP,11,4,beta=3,two\nD,12,5,ghost=0,\n");
    ASSERT_NO_FATAL_FAILURE(writeFile(logPath, content));

    {
        MemTable table(logPath.string());

        ASSERT_NO_FATAL_FAILURE(expectGet(table, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectGet(table, "beta", "two"));
        expectTombstone(table, "ghost");
        EXPECT_EQ(12, table.getMaxWALSeq());
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, content));
}

TEST(MemTableTest, WALRejectsCorruptionBeforeACompleteLaterBatchWithoutTruncatingIt)
{
    const std::filesystem::path logPath("memtable_tests_reject_mid_file_corruption.wal");
    const ScopedPathCleanup cleanup(logPath);
    std::string content = walContent({"1,P,5,5,alpha=3,one\n", "1,P,6,4,beta=3,two\n"});

    // Damage the first committed batch while leaving a later complete batch in
    // place. This is not a torn tail and must not be silently repaired by
    // discarding every byte after the corruption.
    const auto firstOperation = content.find("P,5");
    ASSERT_NE(std::string::npos, firstOperation);
    content[firstOperation] = 'X';
    ASSERT_NO_FATAL_FAILURE(writeFile(logPath, content));

    EXPECT_THROW({ const MemTable table(logPath.string()); }, std::runtime_error);

    std::string contentAfterOpen;
    ASSERT_NO_FATAL_FAILURE(test_support::readFile(logPath, contentAfterOpen));
    EXPECT_EQ(content, contentAfterOpen) << "mid-file corruption must not be mistaken for a disposable WAL tail";
}

TEST(MemTableTest, WALRejectsChecksumMismatchInFinalCommittedBatch)
{
    const std::filesystem::path logPath("memtable_tests_reject_final_checksum_mismatch.wal");
    const ScopedPathCleanup cleanup(logPath);
    std::string content = walContent("1,P,5,5,alpha=3,one\n");
    const auto valuePosition = content.find("one");
    ASSERT_NE(std::string::npos, valuePosition);
    content[valuePosition] ^= 0x01;
    ASSERT_NO_FATAL_FAILURE(writeFile(logPath, content));

    EXPECT_THROW({ const MemTable table(logPath.string()); }, std::runtime_error);
    ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, content));
}

TEST(MemTableTest, WALRejectsDamagedLengthInFinalCommittedBatch)
{
    const std::filesystem::path logPath("memtable_tests_reject_final_length_damage.wal");
    const ScopedPathCleanup cleanup(logPath);
    std::string content = walContent("1,P,5,5,alpha=3,one\n");
    content[kWalHeader.size() + kWalFrameMagic.size()] ^= 0x01;
    ASSERT_NO_FATAL_FAILURE(writeFile(logPath, content));

    EXPECT_THROW({ const MemTable table(logPath.string()); }, std::runtime_error);
    ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, content));
}

TEST(MemTableTest, WALTruncatesZeroFilledTail)
{
    const std::filesystem::path logPath("memtable_tests_zero_filled_tail.wal");
    const ScopedPathCleanup cleanup(logPath);
    const std::string expected = walContent("1,P,5,5,alpha=3,one\n");
    std::string content = expected;
    content.append(4096, '\0');
    ASSERT_NO_FATAL_FAILURE(writeFile(logPath, content));

    {
        const MemTable table(logPath.string());
        ASSERT_NO_FATAL_FAILURE(expectGet(table, "alpha", "one"));
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, expected));
}

TEST(MemTableTest, WALTruncatesGarbageTail)
{
    const std::filesystem::path logPath("memtable_tests_garbage_tail.wal");
    const ScopedPathCleanup cleanup(logPath);
    const std::string expected = walContent("1,P,5,5,alpha=3,one\n");
    std::string content = expected;
    content.append(std::string_view{"\x7fstale bytes from an old block\x01\x02"});
    ASSERT_NO_FATAL_FAILURE(writeFile(logPath, content));

    {
        const MemTable table(logPath.string());
        ASSERT_NO_FATAL_FAILURE(expectGet(table, "alpha", "one"));
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, expected));
}

TEST(MemTableTest, WALDropsEntireIncompleteFinalBatch)
{
    const std::filesystem::path logPath("memtable_tests_restore_incomplete_batch.wal");
    const ScopedPathCleanup cleanup(logPath);
    std::string content = walContent("1,P,5,5,alpha=3,one\n");
    std::string tornFrame = walFrame("2,P,6,4,beta=3,two\nP,7,5,gamma=5,three\n");
    ASSERT_GT(tornFrame.size(), 10);
    tornFrame.resize(tornFrame.size() - 10);
    content += tornFrame;
    ASSERT_NO_FATAL_FAILURE(writeFile(logPath, content));

    {
        MemTable table(logPath.string());

        ASSERT_NO_FATAL_FAILURE(expectGet(table, "alpha", "one"));
        expectMissing(table, "beta");
        expectMissing(table, "gamma");
        EXPECT_EQ(5, table.getMaxWALSeq());
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, walContent("1,P,5,5,alpha=3,one\n")));
}

TEST(MemTableTest, WALRejectsOverflowBatchCountInCommittedFrame)
{
    const std::filesystem::path logPath("memtable_tests_restore_overflow_batch_count.wal");
    const ScopedPathCleanup cleanup(logPath);
    const std::string content = walContent({"1,P,5,5,alpha=3,one\n", "99999999999999999999999,P,6,3,bad=1,x\n"});
    ASSERT_NO_FATAL_FAILURE(writeFile(logPath, content));

    EXPECT_THROW({ const MemTable table(logPath.string()); }, std::runtime_error);
    ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, content));
}

TEST(MemTableTest, WALReplayKeepsLatestValueForDuplicateKey)
{
    const std::filesystem::path logPath("memtable_tests_restore_duplicate.wal");
    const ScopedPathCleanup cleanup(logPath);
    const std::string content = walContent({"1,P,1,1,k=3,old\n", "1,P,2,1,k=3,new\n"});
    ASSERT_NO_FATAL_FAILURE(writeFile(logPath, content));

    {
        MemTable table(logPath.string());

        ASSERT_NO_FATAL_FAILURE(expectGet(table, "k", "new"));
        EXPECT_EQ(2, table.getMaxWALSeq());
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, content));
}

TEST(MemTableTest, DuplicateKeyAndSequenceHasSameReplacementSemanticsAfterReplay)
{
    const std::filesystem::path logPath("memtable_tests_restore_duplicate_key_and_seq.wal");
    const ScopedPathCleanup cleanup(logPath);
    const std::vector<Record> operations{
        {"key", 7, Type::VALUE, "old"},
        {"key", 7, Type::VALUE, "replacement"},
    };

    {
        MemTable table(logPath.string());

        ASSERT_TRUE(table.applyBatch(operations));
        EXPECT_EQ(1u, table.size());
        EXPECT_EQ(entrySize("key", "replacement"), table.size_bytes());
        EXPECT_EQ(7, table.getMaxWALSeq());
        ASSERT_NO_FATAL_FAILURE(expectGet(table, "key", "replacement"));
    }

    {
        const MemTable table(logPath.string());

        EXPECT_EQ(1u, table.size());
        EXPECT_EQ(entrySize("key", "replacement"), table.size_bytes());
        EXPECT_EQ(7, table.getMaxWALSeq());
        ASSERT_NO_FATAL_FAILURE(expectGet(table, "key", "replacement"));
    }
}

TEST(MemTableTest, WALReplayKeepsTombstoneForDeletedKey)
{
    const std::filesystem::path logPath("memtable_tests_restore_tombstone.wal");
    const ScopedPathCleanup cleanup(logPath);

    {
        MemTable table(logPath.string());

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "k", "old", 3));
        ASSERT_NO_FATAL_FAILURE(expectRemove(table, "k", 4));
        expectTombstone(table, "k");
    }

    {
        MemTable table(logPath.string());

        expectTombstone(table, "k");
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, walContent({"1,P,3,1,k=3,old\n", "1,D,4,1,k=0,\n"})));
}

TEST(MemTableTest, WALTruncatesIncompleteTailDuringRestore)
{
    const std::filesystem::path logPath("memtable_tests_restore_truncate_tail.wal");
    const ScopedPathCleanup cleanup(logPath);
    std::string content = walContent("1,P,5,5,alpha=3,one\n");
    std::string tornFrame = walFrame("1,P,6,4,beta=3,two\n");
    tornFrame.resize(tornFrame.size() - 12);
    content += tornFrame;
    ASSERT_NO_FATAL_FAILURE(writeFile(logPath, content));

    {
        MemTable table(logPath.string());

        ASSERT_NO_FATAL_FAILURE(expectGet(table, "alpha", "one"));
        expectMissing(table, "beta");
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, walContent("1,P,5,5,alpha=3,one\n")));
}

TEST(MemTableTest, WALCanAppendAfterTruncatingDamagedTailAndRestoreAgain)
{
    const std::filesystem::path logPath("memtable_tests_restore_append_after_truncate.wal");
    const ScopedPathCleanup cleanup(logPath);
    std::string content = walContent("1,P,5,5,alpha=3,one\n");
    std::string tornFrame = walFrame("1,P,6,4,beta=3,two\n");
    tornFrame.resize(tornFrame.size() - 12);
    content += tornFrame;
    ASSERT_NO_FATAL_FAILURE(writeFile(logPath, content));

    {
        MemTable table(logPath.string());

        ASSERT_NO_FATAL_FAILURE(expectGet(table, "alpha", "one"));
        expectMissing(table, "beta");
        ASSERT_NO_FATAL_FAILURE(expectPut(table, "gamma", "three", 7));
        ASSERT_NO_FATAL_FAILURE(expectGet(table, "gamma", "three"));
    }

    {
        MemTable table(logPath.string());

        ASSERT_NO_FATAL_FAILURE(expectGet(table, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectGet(table, "gamma", "three"));
        expectMissing(table, "beta");
    }

    ASSERT_NO_FATAL_FAILURE(
        expectFileContent(logPath, walContent({"1,P,5,5,alpha=3,one\n", "1,P,7,5,gamma=5,three\n"})));
}

TEST(MemTableTest, WALRejectsOverflowLengthInCommittedFrame)
{
    const std::filesystem::path logPath("memtable_tests_restore_overflow_length.wal");
    const ScopedPathCleanup cleanup(logPath);
    const std::string content = walContent({"1,P,5,5,alpha=3,one\n", "1,P,99999999999999999999999,3,bad=1,x\n"});
    ASSERT_NO_FATAL_FAILURE(writeFile(logPath, content));

    EXPECT_THROW({ const MemTable table(logPath.string()); }, std::runtime_error);
    ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, content));
}

TEST(MemTableTest, WALAppendFormat)
{
    const std::filesystem::path logPath("memtable_tests_wal_append.wal");
    const ScopedPathCleanup cleanup(logPath);

    {
        MemTable table(logPath.string());
        std::string expected(kWalHeader);

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "alpha", "one", 41));
        expected += walFrame("1,P,41,5,alpha=3,one\n");
        ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, expected));

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "beta", "two", 42));
        expected += walFrame("1,P,42,4,beta=3,two\n");
        ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, expected));

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "a,b=c", "v=1,ok", 43));
        expected += walFrame("1,P,43,5,a,b=c=6,v=1,ok\n");
        ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, expected));

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "a", "vvv\n\n\rvvv", 44));
        expected += walFrame("1,P,44,1,a=9,vvv\n\n\rvvv\n");
        ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, expected));
    }
}

TEST(MemTableTest, WALAppendsToExistingLog)
{
    const std::filesystem::path logPath("memtable_tests_existing_append.wal");
    const ScopedPathCleanup cleanup(logPath);

    {
        std::ofstream seed(logPath, std::ios::binary);
        ASSERT_TRUE(seed.is_open()) << "expected seed WAL to open";
        seed << walContent("1,P,98,4,seed=5,value\n");
    }

    {
        MemTable table(logPath.string());
        ASSERT_NO_FATAL_FAILURE(expectPut(table, "next", "record", 99));
    }

    ASSERT_NO_FATAL_FAILURE(
        expectFileContent(logPath, walContent({"1,P,98,4,seed=5,value\n", "1,P,99,4,next=6,record\n"})));
}

TEST(MemTableTest, WALCreatesParentDirectories)
{
    const std::filesystem::path root("memtable_tests_nested_logs");
    const ScopedPathCleanup cleanup(root);
    const std::filesystem::path logPath = root / "child" / "wal.log";

    {
        MemTable table(logPath.string());
        ASSERT_NO_FATAL_FAILURE(expectPut(table, "parent", "created", 7));

        EXPECT_TRUE(std::filesystem::exists(logPath)) << "expected WAL file in nested directory";
        ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, walContent("1,P,7,6,parent=7,created\n")));
    }
}

TEST(MemTableTest, WALRecordsEmptyAndMultiDigitLengths)
{
    const std::filesystem::path logPath("memtable_tests_lengths.wal");
    const ScopedPathCleanup cleanup(logPath);

    {
        MemTable table(logPath.string());
        std::string expected(kWalHeader);

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "empty-value", "", 8));
        expected += walFrame("1,P,8,11,empty-value=0,\n");
        ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, expected));
        ASSERT_NO_FATAL_FAILURE(expectGet(table, "empty-value", ""));

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "tenletters", "0123456789abc", 9));
        expected += walFrame("1,P,9,10,tenletters=13,0123456789abc\n");
        ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, expected));
        ASSERT_NO_FATAL_FAILURE(expectGet(table, "tenletters", "0123456789abc"));

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "", "", 10));
        expected += walFrame("1,P,10,0,=0,\n");
        ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, expected));
        ASSERT_NO_FATAL_FAILURE(expectGet(table, "", ""));
    }
}

TEST(MemTableTest, IteratorIsInvalidForEmptyTable)
{
    const std::filesystem::path logPath("memtable_tests_iterator_empty.wal");
    const ScopedPathCleanup cleanup(logPath);

    {
        const MemTable table(logPath.string());
        const MemTableIterator iterator(table);

        EXPECT_FALSE(iterator.valid());
    }
}

TEST(MemTableTest, IteratorTraversesSortedValuesAndTombstonesThroughBaseInterface)
{
    const std::filesystem::path logPath("memtable_tests_iterator_records.wal");
    const ScopedPathCleanup cleanup(logPath);

    {
        MemTable table(logPath.string());
        ASSERT_NO_FATAL_FAILURE(expectPut(table, "gamma", "three", 30));
        ASSERT_NO_FATAL_FAILURE(expectPut(table, "alpha", "one", 10));
        ASSERT_NO_FATAL_FAILURE(expectPut(table, "beta", "two", 20));
        ASSERT_NO_FATAL_FAILURE(expectRemove(table, "beta", 40));

        MemTableIterator concreteIterator(table);
        Iterator& iterator = concreteIterator;
        size_t emittedRecords = 0;

        ASSERT_NO_FATAL_FAILURE(expectIteratorRecord(iterator, "alpha", 10, Type::VALUE, "one"));
        ++emittedRecords;
        iterator.advance();
        ASSERT_NO_FATAL_FAILURE(expectIteratorRecord(iterator, "beta", 40, Type::TOMBSTONE, ""));
        ++emittedRecords;
        iterator.advance();
        ASSERT_NO_FATAL_FAILURE(expectIteratorRecord(iterator, "beta", 20, Type::VALUE, "two"));
        ++emittedRecords;
        iterator.advance();
        ASSERT_NO_FATAL_FAILURE(expectIteratorRecord(iterator, "gamma", 30, Type::VALUE, "three"));
        ++emittedRecords;
        iterator.advance();
        EXPECT_FALSE(iterator.valid());
        EXPECT_EQ(4, emittedRecords);
        EXPECT_EQ(table.size(), emittedRecords);
    }
}

TEST(MemTableTest, IteratorBecomesInvalidAfterLastRecord)
{
    const std::filesystem::path logPath("memtable_tests_iterator_past_end.wal");
    const ScopedPathCleanup cleanup(logPath);

    {
        MemTable table(logPath.string());
        ASSERT_NO_FATAL_FAILURE(expectPut(table, "only", "value"));

        MemTableIterator iterator(table);
        ASSERT_TRUE(iterator.valid());
        iterator.advance();
        EXPECT_FALSE(iterator.valid());
    }
}
