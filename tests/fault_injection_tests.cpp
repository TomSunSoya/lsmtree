#include <cerrno>
#include <filesystem>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <vector>

#include <gtest/gtest.h>

#include "DB.h"
#include "FaultInjection.h"
#include "MemTable.h"
#include "test_support.h"

using test_support::expectFileContent;
using test_support::ScopedPathCleanup;

namespace
{
constexpr std::string_view kWalHeader{"LWAL\x01", 5};

std::string walContent(const std::string_view records) { return std::string(kWalHeader) + std::string(records); }

// Counts how many syscalls of each kind the engine issued. Useful when you need
// to know which fsync is "the nth" before arming failNth.
struct CallCounter
{
    unsigned writes = 0;
    unsigned fsyncs = 0;

    [[nodiscard]] fault::Policy policy()
    {
        return [this](const fault::Op op, int, std::size_t)
        {
            if (op == fault::Op::Write)
                ++writes;
            else
                ++fsyncs;
            return fault::Decision::proceed();
        };
    }
};
} // namespace

// ---------------------------------------------------------------------------
// Seam self-tests: these check the injector itself, not the engine.
// ---------------------------------------------------------------------------

TEST(FaultInjectionTest, NoPolicyInstalledPassesThrough)
{
    const std::filesystem::path logPath("fault_tests_passthrough.wal");
    const ScopedPathCleanup cleanup(logPath);

    {
        MemTable table(logPath.string());
        EXPECT_TRUE(table.put("k", 1, "v"));
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, walContent("1,P,1,1,k=1,v\n")));
}

TEST(FaultInjectionTest, FailNthTargetsOnlyTheRequestedCall)
{
    const fault::ScopedPolicy policy(fault::failNth(fault::Op::Fsync, 2, EIO));

    errno = 0;
    EXPECT_EQ(-1, fault::fsync(-1));
    EXPECT_EQ(EBADF, errno) << "first fsync should reach the real syscall";

    errno = 0;
    EXPECT_EQ(-1, fault::fsync(-1));
    EXPECT_EQ(EIO, errno) << "second fsync should be the injected failure";

    errno = 0;
    EXPECT_EQ(-1, fault::fsync(-1));
    EXPECT_EQ(EBADF, errno) << "third fsync should reach the real syscall again";
}

TEST(FaultInjectionTest, OneBatchCostsOneWriteAndOneFsyncRegardlessOfRecordCount)
{
    const std::filesystem::path logPath("fault_tests_syscall_count.wal");
    const ScopedPathCleanup cleanup(logPath);

    // Construct first: creating a fresh WAL writes the header and fsyncs it.
    MemTable table(logPath.string());

    CallCounter counter;
    {
        const fault::ScopedPolicy policy(counter.policy());
        ASSERT_TRUE(table.applyBatch({
            {"a", 1, Type::VALUE, "1"},
            {"b", 2, Type::VALUE, "2"},
            {"c", 3, Type::VALUE, "3"},
        }));
    }

    EXPECT_EQ(1u, counter.writes) << "the whole batch should go out in one write";
    EXPECT_EQ(1u, counter.fsyncs) << "a batch has exactly one commit point";
}

TEST(FaultInjectionTest, ScopedPolicyDisarmsOnDestruction)
{
    {
        const fault::ScopedPolicy policy(fault::failEvery(fault::Op::Fsync, EIO));
        errno = 0;
        EXPECT_EQ(-1, fault::fsync(-1));
        EXPECT_EQ(EIO, errno);
    }

    errno = 0;
    EXPECT_EQ(-1, fault::fsync(-1));
    EXPECT_EQ(EBADF, errno) << "policy should be gone once the scope exits";
}

// ---------------------------------------------------------------------------
// Engine invariants under a failing fsync.
// ---------------------------------------------------------------------------

TEST(FaultInjectionTest, ApplyBatchLeavesMemTableUntouchedWhenFsyncFails)
{
    const std::filesystem::path logPath("fault_tests_apply_batch_fsync_fails.wal");
    const ScopedPathCleanup cleanup(logPath);

    const std::vector<Record> operations{
        {"alpha", 10, Type::VALUE, "one"},
        {"beta", 11, Type::VALUE, "two"},
    };

    {
        // Construct the MemTable *before* arming the policy: creating a fresh WAL
        // writes the header and fsyncs it (MemTable.cpp, WALFileWriter ctor). Arming
        // first would make that header fsync "the 1st fsync" and you would be testing
        // the wrong call.
        MemTable table(logPath.string());
        const fault::ScopedPolicy policy(fault::failNth(fault::Op::Fsync, 1, EIO));

        EXPECT_FALSE(table.applyBatch(operations));

        // The commit point never completed, so not one record may have reached the map.
        // These are the assertions that make the test's name true -- move the insert
        // loop in applyBatch above the fsync and they are what goes red.
        EXPECT_EQ(0u, table.size());
        EXPECT_EQ(0u, table.size_bytes());

        std::string value;
        EXPECT_EQ(Result::ABSENT, table.get("alpha", 100, value));
        EXPECT_EQ(Result::ABSENT, table.get("beta", 100, value));

        // The failed batch never reaches the shared in-memory apply path, so it
        // must not advance the maximum committed sequence either.
        EXPECT_EQ(0u, table.getMaxWALSeq());
    }

    // ::write succeeded and only ::fsync failed, so the complete frame is in the file.
    ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, walContent("2,P,10,5,alpha=3,one\nP,11,4,beta=3,two\n")));

    // And because that frame is complete, recovery accepts it: the write reported
    // failure, yet the data comes back. That is the tolerated direction -- reporting
    // success and losing data is the one that never happens.
    {
        MemTable table(logPath.string());

        std::string value;
        EXPECT_EQ(Result::VALUE, table.get("alpha", 100, value));
        EXPECT_EQ("one", value);
        EXPECT_EQ(Result::VALUE, table.get("beta", 100, value));
        EXPECT_EQ("two", value);
        EXPECT_EQ(11u, table.getMaxWALSeq());
    }
}

TEST(FaultInjectionTest, MemTableStaysDeadAfterFsyncFailure)
{
    const std::filesystem::path logPath("fault_tests_after_fsync_failure.wal");
    const ScopedPathCleanup cleanup(logPath);

    MemTable table(logPath.string());
    {
        const fault::ScopedPolicy policy(fault::failNth(fault::Op::Fsync, 1, EIO));
        EXPECT_FALSE(table.applyBatch({{"alpha", 10, Type::VALUE, "one"}}));
    }

    // The policy is gone -- the disk is healthy again -- but WALFileWriter::poisoned_
    // is latched, so the writer refuses every later write. Decision: fail loudly for
    // good rather than keep appending to a file whose real length is now unknown.
    // Recovering from this needs a reopen.
    EXPECT_FALSE(table.put("beta", 11, "two"));

    std::string value;
    EXPECT_EQ(Result::ABSENT, table.get("beta", 100, value));
    EXPECT_EQ(0u, table.size());
}

TEST(FaultInjectionTest, CreatingNewWalFsyncsItsParentDirectory)
{
    const std::filesystem::path root("fault_tests_wal_parent_directory");
    const ScopedPathCleanup cleanup(root);
    const std::filesystem::path logPath = root / "wal" / "wal_0.wal";
    unsigned regularFileFsyncs = 0;
    unsigned directoryFsyncs = 0;

    {
        const fault::ScopedPolicy policy(
            [&](const fault::Op op, const int fd, std::size_t)
            {
                if (op != fault::Op::Fsync)
                    return fault::Decision::proceed();

                struct stat status{};
                if (::fstat(fd, &status) == 0 && S_ISDIR(status.st_mode))
                    ++directoryFsyncs;
                else
                    ++regularFileFsyncs;
                return fault::Decision::proceed();
            });

        const MemTable table(logPath.string());
    }

    EXPECT_GE(regularFileFsyncs, 1u) << "the new WAL header itself must be durable";
    EXPECT_GE(directoryFsyncs, 1u) << "the parent directory entry must also be durable after creating a WAL";
}

TEST(FaultInjectionTest, FlushFailureAfterManifestPublishRejectsLaterWrites)
{
    const std::filesystem::path root("fault_tests_flush_publish_failure");
    const ScopedPathCleanup cleanup(root);
    constexpr uint64_t noAutomaticCompaction = std::numeric_limits<uint64_t>::max();

    {
        DB db(root, 1, noAutomaticCompaction);

        // Fsync order for this path is: WAL batch, SSTable, SSTable directory,
        // MANIFEST, MANIFEST directory, then the new WAL header. Failing the
        // sixth call exercises the dangerous window after the Manifest has
        // published the replacement WAL but before DB switches activeMemTable_.
        {
            const fault::ScopedPolicy policy(fault::failNth(fault::Op::Fsync, 6, EIO));
            EXPECT_TRUE(db.put("committed-before-switch", "one"))
                << "the record is already represented by the published SSTable and Manifest";
        }

        // A DB that cannot finish installing the Manifest's WAL must latch an
        // error state. A later write must not be acknowledged into the old WAL.
        {
            const fault::ScopedPolicy policy(fault::failNth(fault::Op::Fsync, 2, EIO));
            EXPECT_FALSE(db.put("must-not-be-acknowledged", "two"));
        }
    }

    {
        DB reopened(root, std::numeric_limits<uint64_t>::max(), noAutomaticCompaction);
        std::string value;
        ASSERT_TRUE(reopened.get("committed-before-switch", value));
        EXPECT_EQ("one", value);
        EXPECT_FALSE(reopened.get("must-not-be-acknowledged", value));
    }
}
