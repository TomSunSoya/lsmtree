#include "DB.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>

#include "SSTable.h"

namespace
{
using TableNumbers = std::set<uint64_t, std::greater<>>;

void ensureDataDirectory(const std::filesystem::path& directory)
{
    if (!std::filesystem::exists(directory))
    {
        std::filesystem::create_directories(directory);
        return;
    }

    if (!std::filesystem::is_directory(directory))
        throw std::invalid_argument("Data directory is not a directory!");
}

void cleanupOldWALFiles(const std::filesystem::path& directory, const uint64_t currentFileNumber)
{
    if (!std::filesystem::exists(directory) || !std::filesystem::is_directory(directory))
        return;

    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error))
    {
        if (error)
            return;
        if (!entry.is_regular_file(error))
        {
            error.clear();
            continue;
        }

        const auto number = parseNumberedFile(entry.path().filename().string(), kWalPrefix, kWalSuffix);
        if (number && *number < currentFileNumber)
            removeFile(entry.path(), "remove wal file failed");
    }
}

void cleanupUntrackedSSTables(const std::filesystem::path& directory, const TableNumbers& activeTables)
{
    if (!std::filesystem::exists(directory) || !std::filesystem::is_directory(directory))
        return;

    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error))
    {
        if (error)
            return;
        if (!entry.is_regular_file(error))
            continue;
        if (entry.path().extension() != ".sst")
            continue;

        const auto number = parseNumberedFile(entry.path().filename().string(), kSSTablePrefix, kSSTableSuffix);
        if (!number || activeTables.contains(*number))
            continue;
        removeFile(entry.path(), "remove sst file failed");
    }
}

TableNumbers selectLevelZeroCompactionTables(const Manifest& manifest)
{
    const auto& levelZero = manifest.level(0);
    TableNumbers selected;
    for (const TableMeta& table : levelZero)
        selected.insert(table.number);

    std::string levelZeroMin = levelZero.front().minKey;
    std::string levelZeroMax = levelZero.front().maxKey;
    for (const TableMeta& table : levelZero)
    {
        levelZeroMin = std::min(levelZeroMin, table.minKey);
        levelZeroMax = std::max(levelZeroMax, table.maxKey);
    }

    for (const TableMeta& table : manifest.level(1))
    {
        if (table.minKey > levelZeroMax || table.maxKey < levelZeroMin)
            continue;
        selected.insert(table.number);
    }
    return selected;
}

std::vector<std::filesystem::path> tablePaths(const std::filesystem::path& dataDirectory,
                                              const std::vector<uint64_t>& tableNumbers)
{
    std::vector<std::filesystem::path> paths;
    paths.reserve(tableNumbers.size());
    for (const uint64_t number : tableNumbers)
        paths.push_back(sstablePath(dataDirectory, number));
    return paths;
}

std::vector<std::unique_ptr<Iterator>> openTableIterators(const std::vector<std::filesystem::path>& paths)
{
    std::vector<std::unique_ptr<Iterator>> iterators;
    iterators.reserve(paths.size());
    for (const auto& path : paths)
        iterators.push_back(std::make_unique<SSTableIterator>(path));
    return iterators;
}

TableMeta writeCompactionSlice(Manifest& manifest, const std::filesystem::path& dataDirectory,
                               const std::span<Record> records)
{
    const uint64_t outputNumber = manifest.allocateNumber();
    const auto fileSize = SSTable::addRecordToFile(records, sstablePath(dataDirectory, outputNumber));
    return {outputNumber, fileSize, records.front().key, records.back().key};
}

std::vector<TableMeta> writeCompactionOutput(Manifest& manifest, const std::filesystem::path& dataDirectory,
                                             const std::span<Record> records, const uint64_t sliceThreshold)
{
    std::vector<TableMeta> outputTables;
    uint64_t currentSliceBytes = 0;
    size_t sliceBegin = 0;

    for (size_t recordIndex = 0; recordIndex < records.size(); ++recordIndex)
    {
        currentSliceBytes += encodedRecordSize(records[recordIndex]);
        if (currentSliceBytes <= sliceThreshold)
            continue;
        if (recordIndex + 1 < records.size() && records[recordIndex].key == records[recordIndex + 1].key)
            continue;

        outputTables.push_back(
            writeCompactionSlice(manifest, dataDirectory, records.subspan(sliceBegin, recordIndex - sliceBegin + 1)));
        currentSliceBytes = 0;
        sliceBegin = recordIndex + 1;
    }

    if (currentSliceBytes != 0)
        outputTables.push_back(writeCompactionSlice(manifest, dataDirectory, records.subspan(sliceBegin)));
    return outputTables;
}

bool overlapsScanRange(const TableMeta& table, const std::string_view start, const std::string_view end)
{
    return table.maxKey >= start && table.minKey < end;
}

std::vector<std::filesystem::path> selectScanTables(const Manifest& manifest,
                                                    const std::filesystem::path& dataDirectory,
                                                    const std::string_view start, const std::string_view end)
{
    std::vector<std::filesystem::path> paths;
    for (size_t levelNumber = 0; levelNumber < manifest.levelCount(); ++levelNumber)
    {
        for (const TableMeta& table : manifest.level(levelNumber))
        {
            if (overlapsScanRange(table, start, end))
                paths.push_back(sstablePath(dataDirectory, table.number));
        }
    }
    return paths;
}
} // namespace

DB::DB(const std::filesystem::path& dataDirectory, const uint64_t flushThreshold, const uint64_t compactThreshold,
       const uint64_t sliceThreshold, const uint64_t compactBaseThresholdBytes)
    : flushThresholdBytes_(flushThreshold), level0CompactionThreshold_(compactThreshold),
      compactionSliceBytes_(sliceThreshold), compactionBaseThresholdBytes_(compactBaseThresholdBytes)
{
    ensureDataDirectory(dataDirectory);

    manifest_ = std::make_unique<Manifest>(dataDirectory / "MANIFEST");
    const std::filesystem::path sstableDirectory = dataDirectory / "sstable";
    const std::filesystem::path walDirectory = dataDirectory / "wal";
    SSTable::cleanupOrphanedTemps(sstableDirectory);
    cleanupUntrackedSSTables(sstableDirectory, manifest_->allTableNumbers());

    dataDirectory_ = dataDirectory;
    walFilePath_ = walPath(dataDirectory_, manifest_->logNumber());
    cleanupOldWALFiles(walDirectory, manifest_->logNumber());
    activeMemTable_ = std::make_unique<MemTable>(walFilePath_.string());
    nextSeq_ = std::max(manifest_->lastSeq(), activeMemTable_->getMaxWALSeq()) + 1;
}

bool DB::put(const std::string& key, const std::string& value)
{
    WriteBatch batch;
    batch.put(key, value);
    return write(batch);
}

bool DB::get(const std::string_view key, std::string& value) const { return get(key, nextSeq_ - 1, value); }

bool DB::get(const std::string_view key, const uint64_t readSeq, std::string& value) const
{
    const Result result = activeMemTable_->get(key, readSeq, value);
    if (result == Result::ABSENT)
        return searchSSTables(key, readSeq, value);
    return result == Result::VALUE;
}

bool DB::remove(const std::string& key)
{
    WriteBatch batch;
    batch.remove(key);
    return write(batch);
}

bool DB::write(const WriteBatch& batch)
{
    if (writeFailed_)
        return false;

    uint64_t seq = nextSeq_;
    nextSeq_ += batch.records_.size();
    std::vector<Record> records;
    records.reserve(batch.records_.size());
    for (const auto& [key, type, value] : batch.records_)
    {
        records.emplace_back(key, seq++, type, value);
    }

    if (!activeMemTable_->applyBatch(records))
        return false;
    try
    {
        if (activeMemTable_->size_bytes() > flushThresholdBytes_)
            flush();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << std::endl;
    }
    return true;
}

void DB::flush()
{
    if (activeMemTable_->size() == 0)
        return;

    try
    {
        const uint64_t tableNumber = manifest_->allocateNumber();
        const std::filesystem::path tablePath = sstablePath(dataDirectory_, tableNumber);
        if (!std::filesystem::exists(tablePath.parent_path()))
            std::filesystem::create_directories(tablePath.parent_path());

        const uint64_t nextWALNumber = manifest_->allocateNumber();
        manifest_->setLogNumber(nextWALNumber);

        const auto [minKey, maxKey] = SSTable::build(*activeMemTable_, tablePath);
        manifest_->addTable(tableNumber, std::filesystem::file_size(tablePath), minKey, maxKey, 0);
        manifest_->setLastSeq(nextSeq_ - 1);
        manifest_->save();

        const std::filesystem::path oldWALPath = walFilePath_;
        const std::filesystem::path nextWALPath = walPath(dataDirectory_, nextWALNumber);
        activeMemTable_ = std::make_unique<MemTable>(nextWALPath.string());
        walFilePath_ = nextWALPath;

        removeFile(oldWALPath, "remove wal file failed");

        maybeCompact();
    }
    catch (...)
    {
        writeFailed_ = true;
        throw;
    }
}

void DB::compact() { compactLevel(0); }

std::vector<Record> DB::scan(const std::string_view start, const std::string_view end, uint64_t readSeq) const
{
    std::vector<std::unique_ptr<Iterator>> iterators;
    iterators.push_back(std::make_unique<MemTableIterator>(*activeMemTable_));

    const auto inputPaths = selectScanTables(*manifest_, dataDirectory_, start, end);
    for (const auto& path : inputPaths)
        iterators.push_back(std::make_unique<SSTableIterator>(path));

    if (readSeq == std::numeric_limits<uint64_t>::max())
        readSeq = nextSeq_ - 1;
    for (auto& iterator : iterators)
        iterator = std::make_unique<SnapshotIterator>(std::move(iterator), readSeq);

    auto result = mergeAll(iterators);
    latestVisiblePerKey(result);
    auto visibleRecords =
        result | std::views::filter([start, end](const Record& record)
                                    { return record.key >= start && record.key < end && record.type == Type::VALUE; });
    return std::ranges::to<std::vector>(visibleRecords);
}

uint64_t DB::registerSnapshot() const
{
    const uint64_t sequence = nextSeq_ - 1;
    activeSnapshotSequences_->insert(sequence);
    return sequence;
}

Snapshot DB::snapshot() { return {activeSnapshotSequences_, registerSnapshot()}; }

size_t DB::activeSnapshotCount() const { return activeSnapshotSequences_->size(); }

Result DB::searchTable(const uint64_t tableNumber, const std::string_view key, const uint64_t readSeq,
                       std::string& value) const
{
    if (const auto cachedTable = tableCache_.find(tableNumber); cachedTable != tableCache_.end())
        return cachedTable->second->get(key, readSeq, value);

    auto table = std::make_unique<SSTable>(sstablePath(dataDirectory_, tableNumber));
    const Result result = table->get(key, readSeq, value);
    tableCache_.emplace(tableNumber, std::move(table));
    return result;
}

bool DB::searchSSTables(const std::string_view key, const uint64_t readSeq, std::string& value) const
{
    for (const TableMeta& table : manifest_->level(0))
    {
        if (key < table.minKey || key > table.maxKey)
            continue;

        const Result result = searchTable(table.number, key, readSeq, value);
        if (result == Result::VALUE)
            return true;
        if (result == Result::TOMBSTONE)
            return false;
    }

    for (size_t levelNumber = 1; levelNumber < manifest_->levelCount(); ++levelNumber)
    {
        const auto table = manifest_->getTableMeta(levelNumber, key);
        if (!table)
            continue;

        const Result result = searchTable(table->number, key, readSeq, value);
        if (result == Result::VALUE)
            return true;
        if (result == Result::TOMBSTONE)
            return false;
    }
    return false;
}

void DB::compactLevel(const uint64_t levelNumber)
{
    std::vector<uint64_t> removedTables;
    if (levelNumber == 0)
    {
        if (manifest_->level(0).empty())
            return;

        const TableNumbers selectedTables = selectLevelZeroCompactionTables(*manifest_);
        removedTables.assign(selectedTables.begin(), selectedTables.end());
    }
    else
    {
        removedTables = selectHigherLevelCompactionTables(levelNumber);
        if (removedTables.empty())
            return;
    }

    compactRange(removedTables, levelNumber + 1);
}

std::vector<uint64_t> DB::selectHigherLevelCompactionTables(const uint64_t levelNumber)
{
    const auto& levelTables = manifest_->level(levelNumber);
    if (levelTables.empty())
        return {};

    TableMeta selectedTable;
    if (const auto cursor = compactionCursors_.find(levelNumber); cursor == compactionCursors_.end())
    {
        selectedTable = levelTables.front();
    }
    else
    {
        auto table = std::ranges::upper_bound(levelTables, cursor->second, std::less{}, &TableMeta::minKey);
        if (table == levelTables.end())
            table = levelTables.begin();
        selectedTable = *table;
    }
    compactionCursors_.insert_or_assign(levelNumber, selectedTable.maxKey);

    std::vector<TableMeta> selectedTables{selectedTable};
    appendOverlappingTables(selectedTables, levelNumber + 1);

    std::vector<uint64_t> tableNumbers;
    tableNumbers.reserve(selectedTables.size());
    for (const TableMeta& table : selectedTables)
        tableNumbers.push_back(table.number);
    return tableNumbers;
}

void DB::maybeCompact()
{
    while (true)
    {
        if (manifest_->level(0).size() > level0CompactionThreshold_)
        {
            compactLevel(0);
            continue;
        }
        const auto overBudgetLevel = firstOverBudgetLevel();
        if (!overBudgetLevel)
            break;
        compactLevel(*overBudgetLevel);
    }
}

void DB::compactRange(const std::vector<uint64_t>& removedTables, const uint64_t targetLevel)
{
    const std::vector<std::filesystem::path> inputPaths = tablePaths(dataDirectory_, removedTables);
    auto iterators = openTableIterators(inputPaths);

    std::vector<Record> records = mergeAll(iterators);
    retainForCompaction(records, smallestActiveSnapshot());
    const std::vector<TableMeta> outputTables =
        writeCompactionOutput(*manifest_, dataDirectory_, std::span(records), compactionSliceBytes_);

    manifest_->replaceTables(removedTables, outputTables, targetLevel);
    manifest_->save();

    for (const auto& inputPath : inputPaths)
        removeFile(inputPath, "remove old sst file failed");

    for (const uint64_t tableNumber : removedTables)
        tableCache_.erase(tableNumber);
}

uint64_t DB::levelSizeBytes(const uint64_t levelNumber) const
{
    const auto& tables = manifest_->level(levelNumber);
    uint64_t totalBytes = 0;
    for (const TableMeta& table : tables)
        totalBytes += table.size;
    return totalBytes;
}

uint64_t DB::levelCompactionBudget(const uint64_t levelNumber) const
{
    uint64_t budget = compactionBaseThresholdBytes_;
    for (uint64_t level = 2; level <= levelNumber; ++level)
        budget *= 10;
    return budget;
}

std::optional<uint64_t> DB::firstOverBudgetLevel() const
{
    for (uint64_t levelNumber = 1; levelNumber < manifest_->levelCount(); ++levelNumber)
    {
        if (levelCompactionBudget(levelNumber) < levelSizeBytes(levelNumber))
            return levelNumber;
    }
    return std::nullopt;
}

void DB::appendOverlappingTables(std::vector<TableMeta>& tables, const uint64_t nextLevel) const
{
    if (nextLevel >= manifest_->levelCount())
        return;

    const auto& nextLevelTables = manifest_->level(nextLevel);
    tables.reserve(nextLevelTables.size());
    const TableMeta selectedTable = tables.front();
    for (const TableMeta& table : nextLevelTables)
    {
        if (rangesOverlap(table, selectedTable.minKey, selectedTable.maxKey))
            tables.push_back(table);
    }
}

uint64_t DB::smallestActiveSnapshot() const
{
    return activeSnapshotSequences_->empty() ? nextSeq_ - 1 : *activeSnapshotSequences_->begin();
}

Snapshot::Snapshot(Snapshot&& other) noexcept : activeSequences_(std::move(other.activeSequences_)), seq_(other.seq_) {}

Snapshot& Snapshot::operator=(Snapshot&& other) noexcept
{
    if (&other == this)
        return *this;
    release();
    activeSequences_ = std::move(other.activeSequences_);
    seq_ = other.seq_;
    return *this;
}

Snapshot::~Snapshot() { release(); }

void Snapshot::release()
{
    if (const auto activeSequences = activeSequences_.lock())
    {
        const auto sequence = activeSequences->find(seq_);
        if (sequence != activeSequences->end())
            activeSequences->erase(sequence);
    }
    activeSequences_.reset();
}

uint64_t Snapshot::seq() const { return seq_; }

Snapshot::Snapshot(std::weak_ptr<std::multiset<uint64_t>> activeSequences, const uint64_t seq)
    : activeSequences_(std::move(activeSequences)), seq_(seq)
{
}
