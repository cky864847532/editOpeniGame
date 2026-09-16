#ifndef DATACODEC_TEST_FEATURE_DATACODECFEATUREFAILURE_H
#define DATACODEC_TEST_FEATURE_DATACODECFEATUREFAILURE_H

#include "DataCodec/API/Entry/DataCodecDecodeEntry.h"
#include "DataCodec/API/Entry/DataCodecEncodeEntry.h"
#include "DataCodec/Test/Common/DataCodecTestResult.h"
#include "DataCodec/Test/Common/DataCodecAllocationFailure.h"
#include "DataCodec/Test/Adapter/DataCodecTestAdapter.h"
#include "DataCodec/Runtime/Failure/PipelineFailureManagement.h"
#include "DataCodec/Workflow/Leaf/LeafEncodeExecutor.h"
#include "DataCodec/Workflow/Leaf/LeafDecodeExecutor.h"

#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>

namespace datacodec::test {

class FailureRejectingRecordSink final : public IRunRecordSink {
public:
    [[nodiscard]] RunRecordMask Interests() const noexcept override {
        return std::numeric_limits<RunRecordMask>::max();
    }

    void Submit(const RunRecord&) override {
        ++submissions;
        throw std::bad_alloc{};
    }

    std::size_t submissions{0u};
};

[[nodiscard]] inline TestResult RunDataCodecFeatureFailure() {
    TestResult result;
    auto record = MakeCodecFailureRecord(
        CodecErrorCode::EncodeFailure, "allocation-failed", "编码入口", "容量申请失败");
    Require(result,
        record.code == CodecErrorCode::EncodeFailure && !record.textTruncated &&
        std::string_view(record.origin.data()) == "编码入口" &&
        std::string_view(record.message.data()) == "容量申请失败" &&
        !record.requestedBytes && !record.reservedBytes && !record.limitBytes,
        "failure.fixed-record", "fixed failure data must preserve text and absent byte counts");

    record.requestedBytes = 64u;
    record.reservedBytes = 128u;
    record.limitBytes = 160u;
    const auto copied = record;
    const auto moved = std::move(record);
    Require(result,
        copied.requestedBytes == 64u && moved.reservedBytes == 128u && moved.limitBytes == 160u,
        "failure.value-lifetime", "copy and move must retain fixed failure values");

    // 分别覆盖一至四字节字符恰好放满以及在每个字节处截断
    for (const std::string_view character : {"A", "é", "内", "😀"}) {
        for (std::size_t room = 0u; room <= character.size(); ++room) {
            const auto prefix = std::string(255u - room, 'x');
            const auto text = prefix + std::string(character);
            const auto boundary = MakeCodecFailureRecord(
                CodecErrorCode::PipelineFailure, "boundary", "test", text);
            const auto expected = room == character.size() ? text : prefix;
            Require(result,
                std::string_view(boundary.message.data()) == expected &&
                boundary.textTruncated == (room != character.size()) &&
                boundary.message.back() == '\0',
                "failure.utf8-boundary", "truncation must retain complete UTF-8 characters");
        }
    }

    const auto overflow = MakeCodecFailureRecord(
        CodecErrorCode::InvalidInput, std::string(80u, 'r'), std::string(80u, 'o'), "");
    Require(result,
        overflow.textTruncated && std::string_view(overflow.reason.data()).size() == 31u &&
        std::string_view(overflow.origin.data()).size() == 63u && overflow.message.front() == '\0',
        "failure.all-text-fields", "all text fields must be bounded and zero terminated");

    auto sink = std::make_shared<FailureRejectingRecordSink>();
    EncodeRequest encode;
    encode.runRecordSink = sink;
    const auto encoded = Encode(encode);
    Require(result,
        !encoded.success && encoded.failure &&
        encoded.failure->code == CodecErrorCode::InvalidInput && encoded.messages.empty() &&
        sink->submissions == 0u,
        "failure.encode-entry", "invalid encode must return fixed data without invoking logging");

    DecodePackageRequest decode;
    decode.runRecordSink = sink;
    decode.attributeSelection = static_cast<AttributeSelectionMode>(255);
    const auto decoded = DecodePackage(decode);
    Require(result,
        !decoded.success && decoded.failure &&
        decoded.failure->code == CodecErrorCode::InvalidInput && decoded.messages.empty() &&
        sink->submissions == 0u,
        "failure.decode-entry", "invalid decode must return fixed data without invoking logging");

    bool returnedWithoutAllocation = false;
    {
        RejectAllocationsScope reject;
        const auto fixed = MakeCodecFailureRecord(
            CodecErrorCode::PipelineFailure, "allocation-failed", "test", "内存不足");
        const auto invalidEncode = Encode(encode);
        const auto invalidDecode = DecodePackage(decode);
        returnedWithoutAllocation = fixed.message.front() != '\0' &&
            invalidEncode.failure && invalidDecode.failure && rejectedAllocationCount == 0u;
    }
    Require(result, returnedWithoutAllocation,
        "failure.no-allocation-return", "fixed record and invalid entry results must not allocate");

    // 属性目标数组申请失败后继续拒绝全部申请，验证固定结果交付
    TestDataset dataset;
    dataset.pointFields.resize(1u);
    auto adapter = std::make_shared<TestEncodeAdapter>(dataset);
    encode.input.adapter = adapter;
    bool encodeReturned = false;
    {
        RejectAllocationsScope reject;
        const auto allocationFailure = Encode(encode);
        encodeReturned = allocationFailure.failure.has_value() &&
            allocationFailure.failure->code == CodecErrorCode::EncodeFailure &&
            std::string_view(allocationFailure.failure->reason.data()) == "allocation-failed" &&
            allocationFailure.messages.empty() && rejectedAllocationCount == 1u;
    }
    Require(result, encodeReturned,
        "failure.persistent-allocation-failure", "entry must return after allocation rejection without another allocation");

    decode.attributeSelection = AttributeSelectionMode::AllAvailable;
    decode.attributeTargets.push_back({});
    bool decodeReturned = false;
    {
        RejectAllocationsScope reject;
        const auto allocationFailure = DecodePackage(decode);
        decodeReturned = allocationFailure.failure.has_value() &&
            allocationFailure.failure->code == CodecErrorCode::InvalidInput &&
            rejectedAllocationCount == 0u;
    }
    Require(result, decodeReturned,
        "failure.selection-no-allocation", "selection validation must return without allocation");

    // 首错记录与副本在持续拒绝分配时保持可用，记录阶段不调用日志
    DataCodecExecutionResources contextResources(CodecResourceParams{.mode = CodecResourceMode::Fixed, .maxComputeThreads = 1u});
    CodecRunScope contextRun(contextResources);
    EncodeContext encodeContext(contextResources);
    DecodeContext decodeContext(contextResources);
    bool contextsRetainedFirst = false;
    {
        RejectAllocationsScope reject;
        const auto first = MakeCodecFailureRecord(
            CodecErrorCode::InvalidRemap, "operation-failed", "原始阶段", "原始错误");
        const auto firstEncode = encodeContext.RecordFailure(first);
        const auto firstDecode = decodeContext.RecordFailure(first);
        const auto laterEncode = encodeContext.RecordFailure("Cleanup", CodecErrorCode::EncodeFailure, "secondary");
        const auto laterDecode = decodeContext.RecordFailure("Cleanup", CodecErrorCode::DecodeFailure, "secondary");
        contextsRetainedFirst = firstEncode && !firstDecode && !laterEncode && !laterDecode &&
            encodeContext.FirstFailure()->code == CodecErrorCode::InvalidRemap &&
            decodeContext.FirstFailure()->code == CodecErrorCode::InvalidRemap &&
            std::string_view(encodeContext.FirstFailure()->message.data()) == "原始错误" &&
            rejectedAllocationCount == 0u;
    }
    Require(result, contextsRetainedFirst, "failure.context-first-record",
        "context failure publication and copies must retain the original record without allocation");

    auto retainedWorkspace = std::make_shared<DecodeLeafWorkspace>();
    bool sharedCancellation = false;
    {
        DataCodecExecutionResources bindingResources(CodecResourceParams{.mode = CodecResourceMode::Fixed, .maxComputeThreads = 1u});
        CodecRunScope bindingRun(bindingResources);
        {
            RunBinding binding(*retainedWorkspace, bindingResources);
            bindingResources.RequestStop();
            sharedCancellation = retainedWorkspace->StopRequested() && retainedWorkspace->StopToken().stop_requested();
        }
    }
    Require(result, sharedCancellation && !retainedWorkspace->StopToken().stop_possible() &&
        !retainedWorkspace->StopRequested(), "failure.workspace-run-binding",
        "retained workspace must observe root cancellation while bound and retain no execution pointer afterward");

    struct StoppedWorkspace {
        bool stopped{false};
        unsigned cleanupCount{0u};
        void RequestStop() noexcept { stopped = true; }
        bool StopRequested() const noexcept { return stopped; }
        void CleanupOnFailure() noexcept { ++cleanupCount; }
    } workspace;
    DataCodecExecutionResources stageResources(CodecResourceParams{.mode = CodecResourceMode::Fixed, .maxComputeThreads = 1u});
    CodecRunScope stageRun(stageResources);
    EncodeContext stageContext(stageResources);
    bool stageReturned = false;
    {
        RejectAllocationsScope reject;
        FailureScope<EncodeContext, StoppedWorkspace> guard(stageContext, workspace);
        const auto succeeded = guard.Run("HeavyStage", CodecErrorCode::EncodeFailure, [] {
            // 直接调用测试可执行程序中的 operator new，确保发生真实申请拒绝
            auto* bytes = ::operator new(4096u);
            ::operator delete(bytes);
        });
        guard.CleanupIfFailed();
        guard.CleanupIfFailed();
        const auto failure = stageContext.FirstFailure();
        stageReturned = !succeeded && workspace.stopped && workspace.cleanupCount == 1u &&
            failure && std::string_view(failure->reason.data()) == "allocation-failed" &&
            std::string_view(failure->origin.data()) == "HeavyStage" && rejectedAllocationCount == 1u;
    }
    Require(result, stageReturned, "failure.stage-allocation-cleanup",
        "a real failed allocation must preserve its stage, request stop and perform cleanup once");

    // 丰富报告拒绝接收时，固定失败结果保持完整
    RunRecordEmitter report;
    RunRecordInfo reportInfo;
    reportInfo.objectName = "failed operation with a name requiring owned text";
    report.Reset(std::move(reportInfo), sink.get());
    const auto saved = stageContext.FirstFailure();
    report.TryEndFailedRun(*saved);
    bool reportReturned = false;
    {
        RejectAllocationsScope reject;
        report.TryEndFailedRun(*saved);
        reportReturned = stageContext.FirstFailure()->code == saved->code &&
            std::string_view(stageContext.FirstFailure()->origin.data()) == "HeavyStage";
    }
    Require(result, reportReturned, "failure.optional-report",
        "logging exceptions and rejected report allocations must not alter the fixed result");

    RunRecordEmitter retainedMessages;
    for (unsigned index = 0u; index < 32u; ++index) {
        retainedMessages.AddInfo("test", "retained message");
    }
    bool messagesReturned = false;
    {
        RejectAllocationsScope reject;
        const auto messages = retainedMessages.TakeMessages();
        messagesReturned = messages.size() == 32u && messages.front().order == 0u &&
            messages.back().order == 31u && rejectedAllocationCount == 0u;
    }
    Require(result, messagesReturned, "failure.message-transfer",
        "moving retained messages out of an emitter must not allocate sorting storage");

    bool missingErrorReturned = false;
    {
        RejectAllocationsScope reject;
        missingErrorReturned = !validation::AssignError(nullptr,
            "a deliberately long error message requiring heap allocation as a string") &&
            rejectedAllocationCount == 0u;
    }
    Require(result, missingErrorReturned, "failure.absent-error-output",
        "an absent error output must not construct a temporary owned error string");

    LeafEncodeRequest missingContext;
    bool leafReturned = false;
    {
        RejectAllocationsScope reject;
        const auto leaf = LeafEncodeExecutor::Execute(missingContext);
        leafReturned = !leaf.success && leaf.failure && leaf.messages.empty() && rejectedAllocationCount == 0u;
    }
    Require(result, leafReturned, "failure.leaf-missing-context",
        "leaf entry validation must return fixed failure without constructing reporting infrastructure");
    return result;
}

} // DataCodec 测试命名空间

#endif
