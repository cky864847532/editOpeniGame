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
    DataCodecExecutionResources& resources, DecodeSession* session) try {
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
    return ExecutePackageDecodeWorkflow(request, resources, session);
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
    DataCodecExecutionResources resources(request.resources);
    if (!resources.BeginRun()) {
        DecodePackageResult result;
        result.failure = resources.FirstFailure();
        return result;
    }
    auto result = DecodePackageInRun(request, resources, nullptr);
    if (result.failure) { resources.RecordFailure(*result.failure); }
    if (!result.success || resources.Stopped()) { resources.CancelAndWaitRun(); }
    if (!resources.EndRun() || resources.FirstFailure()) {
        result.success = false;
        result.failure = resources.FirstFailure();
    }
    return result;
} catch (const std::bad_alloc&) {
    return MakeDecodeEntryFailure(CodecErrorCode::DecodeFailure, "allocation-failed", "memory allocation failed");
} catch (const std::exception& error) {
    return MakeDecodeEntryFailure(CodecErrorCode::DecodeFailure, "entry-exception", error.what());
} catch (...) {
    return MakeDecodeEntryFailure(CodecErrorCode::DecodeFailure, "entry-exception", "unknown exception");
}

} // namespace datacodec
