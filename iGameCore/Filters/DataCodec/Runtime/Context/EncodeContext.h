#ifndef DATACODEC_RUNTIME_CONTEXT_ENCODECONTEXT_H
#define DATACODEC_RUNTIME_CONTEXT_ENCODECONTEXT_H

#include "DataCodec/Runtime/Failure/FailureCleanable.h"
#include "DataCodec/Runtime/Execution/DataCodecExecutionResources.h"
#include "DataCodec/Common/DataCodecError.h"
#include "DataCodec/Common/DataCodecTypes.h"
#include "DataCodec/API/Adapter/IRunRecordSink.h"
#include "DataCodec/API/Params/CodecControlParams.h"
#include "DataCodec/Codec/Reference/EncodeReferenceFrame.h"
#include "DataCodec/Runtime/Record/RunRecordEmitter.h"
#include "DataCodec/Runtime/Record/RunRecordTimestamp.h"

#include <cassert>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace datacodec {

class TelemetryMemoryTraceRecorder;

struct EncodeContextInitializeResult {
    bool success{true};
    std::string message;

    explicit operator bool() const noexcept { return success; }
};

struct EncodeContext : IFailureCleanable {
    explicit EncodeContext(DataCodecExecutionResources& run) noexcept : resources(run), runRecords(&run) {}
    DataCodecExecutionResources& resources;
    IEncodeAdapter* adapter{nullptr};
    const CodecControlParams* controlParams{nullptr};
    RunRecordEmitter runRecords;
    RunEndRecord runSummary;
    TelemetryMemoryTraceRecorder* memoryTrace{nullptr};
    std::string objectName;
    std::string meshType;
    BlockPath path;
    std::uint32_t frameIndex{0u};
    DataCodecLanguage language{DataCodecLanguage::SimplifiedChinese};
    std::span<const AttributeTarget> attributeTargets;
    TemporalFieldRole attributeTemporalRole{TemporalFieldRole::SingleFrame};
    std::uint32_t attributeKeyFrameIndex{0u};
    TemporalFieldRole geometryTemporalRole{TemporalFieldRole::SingleFrame};
    std::uint32_t geometryKeyFrameIndex{0u};
    EncodeAttributeReferenceFrame attributeKeyFrameReference;
    EncodeGeometryReferenceFrame geometryKeyFrameReference;
    DecodedAttributeCacheSet* currentAttributeReferenceCache{nullptr};
    DecodedGeometryReferenceCache* currentGeometryReferenceCache{nullptr};
    bytestore::ByteStoreSession* referenceByteStoreSession{nullptr};

    // 校验必要字段并初始化运行记录，调用前需先设置各字段
    EncodeContextInitializeResult Initialize(IRunRecordSink* recordSink) {
        memoryTrace = nullptr;
        runRecords.TryExport([&] {
        runRecords.Reset(
            RunRecordInfo{
                .generatedAtUtc = runrecorddetail::MakeTimestampUtc(),
                .runKind = TelemetryRunKind::Encode,
                .objectName = objectName,
                .leafPath = path,
                .meshType = meshType,
                .language = language,
            },
            recordSink);
        });
        runSummary = {};
        failureCleanupCompleted.store(false, std::memory_order_release);

        if (adapter == nullptr) {
            return {false, "DataCodec encode context requires an adapter"};
        }
        return {};
    }

    void AddInfo(std::string origin, std::string text) {
        runRecords.AddInfo(std::move(origin), std::move(text));
    }

    void AddWarning(std::string origin, std::string text) {
        runRecords.AddWarning(std::move(origin), std::move(text));
    }

    void AddError(std::string origin, std::string text) {
        runRecords.AddError(std::move(origin), std::move(text));
    }

    bool RecordFailure(
        const std::string_view stageName,
        const CodecErrorCode code,
        const std::string_view message) noexcept {
        return RecordFailure(MakeCodecFailureRecord(code, "operation-failed", stageName, message));
    }

    bool RecordFailure(const CodecFailureRecord& record) noexcept {
        return resources.RecordFailure(record);
    }

    bool RecordFailure(
        const std::string_view stageName,
        const CodecErrorCode code,
        const std::string& message) noexcept {
        return RecordFailure(stageName, code, std::string_view(message));
    }

    bool RecordFailure(
        const std::string_view stageName,
        const CodecErrorCode code,
        const char* message) noexcept {
        assert(message != nullptr);
        return RecordFailure(stageName, code, std::string_view(message));
    }

    [[nodiscard]] bool HasFailure() const noexcept {
        return resources.FirstFailure().has_value();
    }

    [[nodiscard]] std::optional<CodecFailureRecord> FirstFailure() const noexcept {
        return resources.FirstFailure();
    }

    void ResetCurrentAttributeReferenceCache() noexcept {
        if (currentAttributeReferenceCache != nullptr) {
            currentAttributeReferenceCache->Reset();
        }
    }

    void ResetCurrentGeometryReferenceCache() noexcept {
        if (currentGeometryReferenceCache != nullptr) {
            currentGeometryReferenceCache->Reset();
        }
    }

    void CleanupOnFailure() noexcept override {
        if (failureCleanupCompleted.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        ResetCurrentAttributeReferenceCache();
        ResetCurrentGeometryReferenceCache();
        if (adapter != nullptr) {
            try {
                adapter->Abort();
            } catch (...) {
            }
        }
    }

    std::atomic_bool failureCleanupCompleted{false};
};

} // namespace datacodec

#endif
