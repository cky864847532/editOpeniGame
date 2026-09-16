#include "DataCodec/API/Entry/DataCodecDecodeEntry.h"

#include "DataCodec/Workflow/Decode/PackageDecodeWorkflow.h"
#include "DataCodec/Workflow/Session/CodecRunEntry.h"

namespace datacodec {

namespace {

DecodePackageResult MakeDecodeEntryFailure(
    const CodecErrorCode code,
    const std::string_view reason,
    const std::string_view text) noexcept {
    DecodePackageResult result;
    result.failure = MakeCodecFailureRecord(code, reason, "DataCodecDecodeEntry", text);
    return result;
}

} // 匿名命名空间

DecodePackageResult DecodePackageInRun(const DecodePackageRequest& request,
    DataCodecExecutionResources& resources, DecodeSession* session, const FramePackage* metadata) try {
    if (request.attributeSelection != AttributeSelectionMode::Explicit &&
        !request.attributeTargets.empty()) {
        return MakeDecodeEntryFailure(
            CodecErrorCode::InvalidInput,
            "decode.attribute-selection",
            "explicit attribute targets require AttributeSelectionMode::Explicit");
    }
    if (request.attributeSelection != AttributeSelectionMode::None &&
        request.attributeSelection != AttributeSelectionMode::AllAvailable &&
        request.attributeSelection != AttributeSelectionMode::Explicit) {
        return MakeDecodeEntryFailure(
            CodecErrorCode::InvalidInput,
            "decode.attribute-selection",
            "decode request contains an unknown attribute selection mode");
    }
    auto result = ExecutePackageDecodeWorkflow(request, resources, session, metadata);
    result.inputMemory = request.input.ObserveMemory();
    return result;
} catch (const std::bad_alloc&) {
    return MakeDecodeEntryFailure(
        CodecErrorCode::DecodeFailure, "allocation-failed", "memory allocation failed");
} catch (const std::exception& exception) {
    return MakeDecodeEntryFailure(
        CodecErrorCode::DecodeFailure, "entry-exception", exception.what());
} catch (...) {
    return MakeDecodeEntryFailure(
        CodecErrorCode::DecodeFailure, "entry-exception", "unknown exception");
}

DecodePackageResult DecodePackage(const DecodePackageRequest& request) try {
    if (request.attributeSelection != AttributeSelectionMode::Explicit && !request.attributeTargets.empty()) {
        return MakeDecodeEntryFailure(CodecErrorCode::InvalidInput, "decode.attribute-selection",
            "explicit attribute targets require AttributeSelectionMode::Explicit");
    }
    if (request.attributeSelection != AttributeSelectionMode::None &&
        request.attributeSelection != AttributeSelectionMode::AllAvailable &&
        request.attributeSelection != AttributeSelectionMode::Explicit) {
        return MakeDecodeEntryFailure(CodecErrorCode::InvalidInput, "decode.attribute-selection",
            "decode request contains an unknown attribute selection mode");
    }
    if (request.stopToken.stop_requested()) {
        auto result = MakeDecodeEntryFailure(CodecErrorCode::DecodeFailure, "cancelled", "decode cancelled");
        result.cancelled = true;
        result.failure->cancelled = true;
        return result;
    }
    DataCodecExecutionResources resources(request.resources);
    if (!resources.BeginRun()) {
        DecodePackageResult result;
        result.failure = resources.FirstFailure();
        return result;
    }
    std::stop_callback stop(request.stopToken, [&resources] { resources.RequestStop(); });
    auto result = DecodePackageInRun(request, resources, nullptr);
    if (result.failure) { resources.RecordFailure(*result.failure); }
    if (!result.success || resources.Stopped()) { resources.CancelAndWaitRun(); }
    if (!resources.EndRun() || resources.FirstFailure()) {
        result.success = false;
        result.failure = resources.FirstFailure();
    }
    if (!result.success) { result.output = {}; }
    result.cancelled = result.cancelled || (result.failure && result.failure->cancelled);
    return result;
} catch (const std::bad_alloc&) {
    return MakeDecodeEntryFailure(CodecErrorCode::DecodeFailure, "allocation-failed", "memory allocation failed");
} catch (const std::exception& error) {
    return MakeDecodeEntryFailure(CodecErrorCode::DecodeFailure, "entry-exception", error.what());
} catch (...) {
    return MakeDecodeEntryFailure(CodecErrorCode::DecodeFailure, "entry-exception", "unknown exception");
}

} // namespace datacodec
