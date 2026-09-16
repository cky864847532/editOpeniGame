#include "DataCodec/API/Entry/InspectEncodedInput.h"
#include "Codec/iGameWasmDataCodecBridge.h"


#include "DataCodec/Filter/Telemetry/iGameDataCodecTelemetryCapture.h"
#include "DataCodec/Runtime/Record/RunRecordSubmit.h"
#include "Codec/iGameWasmDataCodecTiming.h"
#include "DataCodec/Platform/Wasm/WasmBrowserFileByteRangeReader.h"
#include "DataCodec/Platform/Wasm/WasmRuntime.h"


#include <exception>
#include <filesystem>
#include <sstream>
#include <utility>

IGAME_NAMESPACE_BEGIN

namespace {

void SubmitWasmRunError(
    ::datacodec::IRunRecordSink* sink,
    const std::string& text) {
    ::datacodec::SubmitRunMessage(
        sink,
        ::datacodec::TelemetryMessageRecord{
            .severity = ::datacodec::TelemetryMessageSeverity::Error,
            .origin = "iGameWasmDataCodecBridge",
            .text = text,
        });
}

void CleanupFailedWasmDecodeSession(
    iGameWasmDataCodecDecodeResult& result) noexcept {
    if (result.session != nullptr) {
        try {
            result.session->Reset();
        } catch (...) {
        }
        result.session.reset();
    }
    result.decodeResult.success = false;
    result.decodeResult.output = nullptr;
    result.output = nullptr;
    result.success = false;
}

void SetWasmDecodeError(
    iGameWasmDataCodecDecodeResult& result,
    ::datacodec::IRunRecordSink* runRecordSink,
    std::string text) {
    result.error = std::move(text);
    if (!result.decodeResult.failure) {
        result.decodeResult.failure = ::datacodec::MakeCodecFailureRecord(
            ::datacodec::CodecErrorCode::DecodeFailure, "wasm-bridge", "iGameWasmDataCodecBridge", result.error);
    }
    SubmitWasmRunError(runRecordSink, result.error);
}

} // 匿名命名空间

std::future<void> SubmitiGameWasmDataCodecTask(std::function<void()> task) {
    // 顶层请求使用自有后台 driver，块计算由请求内部资源根执行
    return std::async(std::launch::async, std::move(task));
}

bool ResolveiGameWasmPackageSourceIdentity(
    const ::datacodec::EncodedInput& input,
    ::datacodec::DecodeSourceIdentity& sourceIdentity,
    std::string* error) {
    const auto inspection = ::datacodec::InspectEncodedInput(input);
    if (!inspection.success) {
        if (error) { *error = inspection.failure ? ::datacodec::FormatCodecFailure(*inspection.failure) : "input inspection failed"; }
        sourceIdentity = {};
        return false;
    }
    sourceIdentity = std::move(inspection.sourceIdentity);
    if (error != nullptr) { error->clear(); }
    return true;
}

bool ResolveiGameWasmDataCodecFileSourceIdentity(
    const std::string& filePath,
    ::datacodec::DecodeSourceIdentity& sourceIdentity,
    std::string* error) {
    if (filePath.empty()) {
        sourceIdentity = {};
        return ::datacodec::validation::AssignError(
            error,
            "DataCodec WASM file path is empty");
    }
    return ResolveiGameWasmPackageSourceIdentity(::datacodec::EncodedInput::File(filePath), sourceIdentity, error);
}

iGameWasmDataCodecDecodeResult DecodeiGameWasmDataCodec(
    iGameWasmDataCodecDecodeRequest request) {
    iGameWasmDataCodecDecodeResult result;
    const auto exportDiagnostics = [&](auto&& prepare) noexcept {
        try { prepare(); }
        catch (...) { result.diagnosticsIncomplete = true; }
    };
    bool completedSuccessfully = false;
    struct FailureCleanupScope {
        iGameWasmDataCodecDecodeResult& result;
        bool& completedSuccessfully;

        ~FailureCleanupScope() noexcept {
            if (!completedSuccessfully) {
                CleanupFailedWasmDecodeSession(result);
            }
        }
    } failureCleanup{result, completedSuccessfully};
    if (!request.input) {
        SetWasmDecodeError(
            result,
            request.runRecordSink.get(),
            "DataCodec WASM decode requires an input reader");
        return result;
    }
    const auto inspection = ::datacodec::InspectEncodedInput(request.input);
    if (!inspection.success) {
        result.decodeResult.failure = inspection.failure;
        SetWasmDecodeError(result, request.runRecordSink.get(), inspection.failure
            ? ::datacodec::FormatCodecFailure(*inspection.failure) : "input inspection failed");
        return result;
    }
    auto inspectedIdentity = inspection.sourceIdentity;
    if (request.sourceIdentity.IsStable() && request.sourceIdentity != inspectedIdentity) {
        SetWasmDecodeError(
            result,
            request.runRecordSink.get(),
            "DataCodec WASM source identity does not match the package header");
        return result;
    }
    request.sourceIdentity = std::move(inspectedIdentity);
    result.cacheIdentityAvailable = true;


    auto controls = ::datacodec::wasm::MakeWasmDecodeConfiguration(
        request.enableReuseCache);
    if (request.enableEncodedInputCache.has_value()) {
        controls.encodedInputCachePolicy.enabled = *request.enableEncodedInputCache;
    }

    result.encodedInputCacheEnabled = controls.encodedInputCachePolicy.enabled;

    iGameDataCodecTelemetryCapture recordSinks(std::move(request.runRecordSink));
    recordSinks.CaptureSessions(
        ::datacodec::kRunLifecycleRecordMask |
        ::datacodec::RunRecordKind::StageTiming);
    auto runRecordSink = recordSinks.Sink();
    result.session = std::make_shared<DataCodecDataObjectDecodeSession>();
    result.decodeResult = result.session->Open({
        .input = std::move(request.input),
        .inputSourceIdentity = request.sourceIdentity,
        .controlParams = &controls.controlParams,
        .configurationSource = &controls.source,
        .language = controls.language,
        .encodedInputCachePolicy = controls.encodedInputCachePolicy,
        .resources = request.resources,
        .runRecordSink = runRecordSink,
    });
    result.encodedInputCacheStatsAfter = result.session->InputCacheStatistics();
    result.sourceIdentity = request.sourceIdentity;
    result.output = result.decodeResult.success
        ? result.decodeResult.output
        : DataObject::Pointer{};
    exportDiagnostics([&] {
        result.timingDetail = BuildiGameWasmTopologyTimingDetail(recordSinks.SnapshotCompletedTelemetrySessions());
    });
    if (result.output == nullptr) {
        const auto readError = result.decodeResult.failure ? ::datacodec::FormatCodecFailure(*result.decodeResult.failure) : std::string{};
        if (!readError.empty()) {
            SetWasmDecodeError(result, runRecordSink.get(), readError);
        } else if (result.decodeResult.messages.empty()) {
            SetWasmDecodeError(
                result,
                runRecordSink.get(),
                "DataCodec WASM decode returned null");
        } else {
            result.error = result.decodeResult.messages.back().text;
        }
        return result;
    }

    exportDiagnostics([&] {
    std::ostringstream cacheTiming;
    cacheTiming << "cache-identity=" << (result.cacheIdentityAvailable ? 1 : 0)
                << "; encoded-input-cache=" << (result.encodedInputCacheEnabled ? 1 : 0)
                << "; encoded-cache-lookups-delta="
                << (result.encodedInputCacheStatsAfter.lookups -
                    result.encodedInputCacheStatsBefore.lookups)
                << "; encoded-cache-hits-delta="
                << (result.encodedInputCacheStatsAfter.hits -
                    result.encodedInputCacheStatsBefore.hits)
                << "; encoded-cache-misses-delta="
                << (result.encodedInputCacheStatsAfter.misses -
                    result.encodedInputCacheStatsBefore.misses)
                << "; encoded-cache-stores-delta="
                << (result.encodedInputCacheStatsAfter.stores -
                    result.encodedInputCacheStatsBefore.stores)
                << "; encoded-cache-evictions-delta="
                << (result.encodedInputCacheStatsAfter.evictions -
                    result.encodedInputCacheStatsBefore.evictions)
                << "; encoded-cache-resident-inputs="
                << result.encodedInputCacheStatsAfter.residentInputs;
    if (!result.timingDetail.empty()) { result.timingDetail += "; "; }
    result.timingDetail += cacheTiming.str();

    });
    result.diagnosticsIncomplete |= recordSinks.DiagnosticsIncomplete();
    result.success = true;
    completedSuccessfully = true;
    return result;
}

iGameWasmDataCodecDecodeResult DecodeiGameWasmDataCodecFile(
    const std::string& filePath,
    const bool enableReuseCache,
    const std::optional<bool> enableEncodedInputCache,
    std::shared_ptr<::datacodec::IRunRecordSink> runRecordSink,
    ::datacodec::DecodeSourceIdentity sourceIdentity,
    ::datacodec::CodecResourceParams resources) {
    if (filePath.empty()) {
        iGameWasmDataCodecDecodeResult result;
        SetWasmDecodeError(
            result,
            runRecordSink.get(),
            "DataCodec WASM file path is empty");
        return result;
    }
    return DecodeiGameWasmDataCodec(iGameWasmDataCodecDecodeRequest{
        .input = ::datacodec::EncodedInput::File(
            std::filesystem::path(filePath)),
        .sourceIdentity = std::move(sourceIdentity),
        .enableReuseCache = enableReuseCache,
        .enableEncodedInputCache = enableEncodedInputCache,
        .resources = resources,
        .runRecordSink = std::move(runRecordSink),
    });
}

iGameWasmDataCodecDecodeResult DecodeiGameWasmDataCodecMemory(
    std::shared_ptr<const void> inputOwner,
    const std::span<const std::uint8_t> bytes,
    const bool enableReuseCache,
    std::shared_ptr<::datacodec::IRunRecordSink> runRecordSink,
    ::datacodec::CodecResourceParams resources) {
    if (inputOwner == nullptr || bytes.empty()) {
        iGameWasmDataCodecDecodeResult result;
        SetWasmDecodeError(
            result,
            runRecordSink.get(),
            "DataCodec WASM memory input requires a nonempty retained owner");
        return result;
    }
    return DecodeiGameWasmDataCodec(iGameWasmDataCodecDecodeRequest{
        .input = ::datacodec::EncodedInput::Memory(std::move(inputOwner), bytes),
        .enableReuseCache = enableReuseCache,
        .resources = resources,
        .runRecordSink = std::move(runRecordSink),
    });
}

iGameWasmDataCodecDecodeResult DecodeiGameWasmBrowserFile(
    const std::uint32_t browserFileId,
    const std::uint64_t browserFileSize,
    const bool enableReuseCache,
    const std::optional<bool> enableEncodedInputCache,
    std::shared_ptr<::datacodec::IRunRecordSink> runRecordSink,
    ::datacodec::CodecResourceParams resources) try {
    return DecodeiGameWasmDataCodec(iGameWasmDataCodecDecodeRequest{
        .input = ::datacodec::EncodedInput::BrowserFile(browserFileId, browserFileSize),
        .enableReuseCache = enableReuseCache,
        .enableEncodedInputCache = enableEncodedInputCache,
        .resources = resources,
        .runRecordSink = std::move(runRecordSink),
    });
} catch (const std::exception& exception) {
    iGameWasmDataCodecDecodeResult result;
    SetWasmDecodeError(result, runRecordSink.get(), exception.what());
    return result;
} catch (...) {
    iGameWasmDataCodecDecodeResult result;
    SetWasmDecodeError(result, runRecordSink.get(), "browser file input construction failed");
    return result;
}

IGAME_NAMESPACE_END
