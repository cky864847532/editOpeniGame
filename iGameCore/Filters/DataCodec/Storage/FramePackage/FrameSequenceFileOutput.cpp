#include "DataCodec/Storage/FramePackage/FrameSequenceFileOutput.h"
#include "DataCodec/Storage/FramePackage/FramePackageSeries.h"
#include "DataCodec/Validation/Common/DataCodecValidation.h"
#include <atomic>
#include <chrono>
#include <set>

namespace datacodec {

struct FrameSequenceFileOutput::Impl {
    FrameSequenceFileTarget target;
    std::filesystem::path directory, staging;
    std::string stem;
    std::set<std::uint32_t> indices;
    struct Entry { std::filesystem::path staged, final; bool published{false}; };
    struct Backup { std::filesystem::path original, saved; bool moved{false}; };
    std::vector<Entry> entries;
    std::vector<Backup> backups;
    bool committed{false}, restoreFailed{false};

    explicit Impl(FrameSequenceFileTarget value) : target(std::move(value)) {
        directory = target.path.parent_path();
        if (directory.empty()) { directory = std::filesystem::current_path(); }
        auto normalized = target.path;
        normalized.replace_extension(".igc");
        FramePackagePathInfo parsed;
        ParseFramePackagePath(normalized, parsed);
        stem = NormalizeFramePackageSeriesStem(parsed.seriesStem);
    }

    bool Prepare(std::string* error) {
        if (!staging.empty()) { return true; }
        if (target.path.empty() || stem.empty() || target.indexWidth < 1 || target.indexWidth > 10) {
            return validation::AssignError(error, "invalid frame sequence file naming description");
        }
        std::filesystem::create_directories(directory);
        static std::atomic_uint64_t next{0u};
        for (int attempt = 0; attempt < 16; ++attempt) {
            const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
            auto candidate = directory / (".datacodec-sequence-" + std::to_string(nonce) + "-" + std::to_string(next.fetch_add(1u)));
            if (std::filesystem::create_directory(candidate)) { staging = std::move(candidate); return true; }
        }
        return validation::AssignError(error, "could not create a unique frame sequence staging directory");
    }

    bool Rollback() noexcept {
        std::error_code code;
        bool restored = true;
        for (auto& entry : entries) {
            if (entry.published) {
                std::filesystem::remove(entry.final, code);
                if (code) { restored = false; }
                else { entry.published = false; }
            }
        }
        for (auto& backup : backups) {
            if (backup.moved) {
                std::filesystem::rename(backup.saved, backup.original, code);
                if (code) { restored = false; }
                else { backup.moved = false; }
            }
        }
        restoreFailed = !restored;
        return restored;
    }

    ~Impl() {
        if (!committed) { Rollback(); }
        if (!staging.empty() && !restoreFailed) {
            std::error_code ignored;
            std::filesystem::remove_all(staging, ignored);
        }
    }
};

FrameSequenceFileOutput::FrameSequenceFileOutput(FrameSequenceFileTarget target)
    : m_impl(std::make_unique<Impl>(std::move(target))) {}
FrameSequenceFileOutput::~FrameSequenceFileOutput() = default;

std::unique_ptr<FileByteRangeOutput> FrameSequenceFileOutput::OpenFrame(
    std::uint32_t index, std::filesystem::path& finalPath, std::string* error) {
    auto& state = *m_impl;
    if (!state.Prepare(error) || !state.indices.insert(index).second) {
        if (error && error->empty()) { *error = "duplicate frame sequence output index"; }
        return {};
    }
    const auto name = BuildFramePackagePath(state.stem, index, state.target.indexWidth);
    finalPath = state.target.path.parent_path() / name;
    const auto staged = state.staging / name;
    state.entries.push_back({staged, state.directory / name});
    return std::make_unique<FileByteRangeOutput>(staged);
}

bool FrameSequenceFileOutput::Commit(std::string* error) {
    auto& state = *m_impl;
    if (state.committed || state.entries.empty()) {
        return validation::AssignError(error, "frame sequence has no pending output to commit");
    }
    try {
        const auto backupDirectory = state.staging / "previous";
        std::filesystem::create_directory(backupDirectory);
        for (const auto& entry : std::filesystem::directory_iterator(state.directory)) {
            FramePackagePathInfo candidate;
            if (entry.is_regular_file() && ParseFramePackagePath(entry.path(), candidate) &&
                candidate.hasFrameIndex && NormalizeFramePackageSeriesStem(candidate.seriesStem) == state.stem) {
                state.backups.push_back({entry.path(), backupDirectory / entry.path().filename()});
            }
        }
        // 在任何改名之前完成列表分配，回滚只使用已有路径
        for (auto& backup : state.backups) {
            std::filesystem::rename(backup.original, backup.saved);
            backup.moved = true;
        }
        for (auto& entry : state.entries) {
            std::filesystem::rename(entry.staged, entry.final);
            entry.published = true;
        }
        state.committed = true;
        return true;
    } catch (const std::exception& exception) {
        const bool restored = state.Rollback();
        return validation::AssignError(error, std::string(exception.what()) +
            (restored ? "; prior sequence restored" : "; rollback incomplete; prior files retained in " + state.staging.string()));
    }
}

} // 命名空间 datacodec
