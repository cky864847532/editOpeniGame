#ifndef DATACODEC_RUNTIME_CONTEXT_DECODECONTEXT_H
#define DATACODEC_RUNTIME_CONTEXT_DECODECONTEXT_H

#include "DataCodec/API/Adapter/IRunRecordSink.h"
#include "DataCodec/Workflow/Decode/IDecodeAdapter.h"
#include "DataCodec/API/Params/CodecParamDefaults.h"
#include "DataCodec/Common/DataCodecTypes.h"
#include "DataCodec/Runtime/Failure/FailureCleanable.h"
#include "DataCodec/Runtime/Execution/DataCodecExecutionResources.h"
#include "DataCodec/Codec/Reference/DecodedReference.h"
#include "DataCodec/Common/DataCodecError.h"
#include "DataCodec/Storage/LeafPackage/LeafPackage.h"
#include "DataCodec/Runtime/Workspace/DecodeLeafWorkspace.h"
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
namespace datacodec {

class TelemetryMemoryTraceRecorder;

struct DecodeContextInitializeResult {
    bool success{true};
    std::string message;

    explicit operator bool() const noexcept { return success; }
};

struct DecodeContext : IFailureCleanable {
    explicit DecodeContext(DataCodecExecutionResources& run) noexcept : resources(run), runRecords(&run) {}
    DataCodecExecutionResources& resources;
    IDecodeAdapter* adapter{nullptr};
    const LeafPackage* leafPackage{nullptr};
    DecodedAttributeReference* attributeKeyFrameReference{nullptr};
    DecodedGeometryReference* geometryKeyFrameReference{nullptr};
    DecodedGeometryReferenceCache* currentGeometryReferenceCache{nullptr};
    DecodedTopologyReferenceCacheStore* topologyReferenceStore{nullptr};
    std::string topologyReferenceKey;
    std::uint32_t frameIndex{0u};
    DataCodecLanguage language{DataCodecLanguage::SimplifiedChinese};
    AttributeSelectionMode attributeSelection{AttributeSelectionMode::None};
    std::span<const AttributeTarget> attributeTargets;
    AttributeDecodeRequestMode attributeRequestMode{AttributeDecodeRequestMode::DecodeAndCommit};
    RunRecordEmitter runRecords;
    RunEndRecord runSummary;
    TelemetryMemoryTraceRecorder* memoryTrace{nullptr};

    // 校验必要字段并初始化运行记录，调用前需先设置各字段
    DecodeContextInitializeResult Initialize(IRunRecordSink* recordSink) {
        memoryTrace = nullptr;
        runRecords.TryExport([&] {
        runRecords.Reset(
            RunRecordInfo{
                .generatedAtUtc = runrecorddetail::MakeTimestampUtc(),
                .runKind = TelemetryRunKind::Decode,
                .objectName = leafPackage != nullptr ? leafPackage->path : std::string{},
                .leafPath = leafPackage != nullptr ? leafPackage->path : BlockPath{},
                .language = language,
            },
            recordSink);
        });
        runSummary = {};
        failureCleanupCompleted.store(false, std::memory_order_release);

        if (adapter == nullptr) {
            return {false, "DataCodec decode context requires an adapter"};
        }
        if (leafPackage == nullptr) {
            return {false, "DataCodec decode context requires an leaf package"};
        }
        return {};
    }

    void AddInfo(std::string origin, std::string text) {
        runRecords.AddInfo(std::move(origin), std::move(text));
    }

    void AddWarning(std::string origin, std::string text) {
        runRecords.AddWarning(std::move(origin), std::move(text));
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

    void AddError(std::string origin, std::string text) {
        runRecords.AddError(std::move(origin), std::move(text));
    }

    void CleanupOnFailure() noexcept override {
        if (failureCleanupCompleted.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
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
