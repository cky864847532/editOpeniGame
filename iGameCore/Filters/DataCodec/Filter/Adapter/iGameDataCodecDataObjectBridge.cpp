#include "DataCodec/Filter/Adapter/iGameDataCodecDataObjectBridge.h"
#include "DataCodec/Filter/Adapter/iGameDecodeAdapter.h"
#include "DataCodec/Filter/Adapter/iGameFramePackageDecodeAssembly.h"
#include "DataCodec/Runtime/Record/RunRecordSubmit.h"
#include "DataCodec/Validation/Filter/FilterCommitValidator.h"
#include "DataCodec/Validation/Workflow/DecodeValidationLifecycle.h"

#include <mutex>

IGAME_NAMESPACE_BEGIN
namespace {

DataCodecDataObjectDecodeResult ConvertPackageDecodeResult(
    ::datacodec::DecodePackageResult decoded, DataObject::Pointer output,
    ::datacodec::IRunRecordSink* sink) {
    DataCodecDataObjectDecodeResult result;
    result.success = decoded.success && output != nullptr;
    result.failure = decoded.failure;
    result.output = std::move(output);
    result.inputBytes = decoded.inputBytes;
    result.inputMemory = decoded.inputMemory;
    result.messages = std::move(decoded.messages);
    const auto validation = ::datacodec::validation::FilterCommitValidator::ValidateDecodedOutput(
        decoded.success, result.output != nullptr);
    if ((!validation && decoded.success) || (decoded.failure && result.messages.empty())) {
        const auto message = ::datacodec::MakeCodecTelemetryMessage("DataCodecDataObjectBridge",
            decoded.failure ? decoded.failure->code : ::datacodec::CodecErrorCode::DecodeFailure,
            decoded.failure ? ::datacodec::FormatCodecFailure(*decoded.failure) : validation.message);
        ::datacodec::SubmitRunMessage(sink, message);
        ::datacodec::AppendRetainedTelemetryMessage(result.messages, message);
    }
    return result;
}

} // 匿名命名空间

struct DataCodecDataObjectDecodeSession::Impl {
    mutable std::recursive_mutex mutex;
    iGameFramePackageDecodeAssembly frameAssembly;
    ::datacodec::PackageDecodeSession session;
    DataObject::Pointer output;
    bool decodedFramePackage{false};

    void Reset() noexcept {
        session.Reset();
        frameAssembly.AbortFramePackage();
        output = nullptr;
        decodedFramePackage = false;
    }
};

DataCodecDataObjectDecodeSession::DataCodecDataObjectDecodeSession() : m_impl(std::make_unique<Impl>()) {}
DataCodecDataObjectDecodeSession::~DataCodecDataObjectDecodeSession() = default;
DataCodecDataObjectDecodeSession::DataCodecDataObjectDecodeSession(DataCodecDataObjectDecodeSession&&) noexcept = default;
DataCodecDataObjectDecodeSession& DataCodecDataObjectDecodeSession::operator=(DataCodecDataObjectDecodeSession&&) noexcept = default;

DataCodecDataObjectDecodeResult DataCodecDataObjectDecodeSession::Open(
    const DataCodecDataObjectDecodeRequest& request) {
    Reset();
    std::lock_guard lock(m_impl->mutex);
    auto& state = *m_impl;
    auto decoded = state.session.Open({
        .decode = {
            .input = request.input,
            .cellTypeMapping = std::make_shared<iGameCellTypeMapping>(),
            .requestedFrameIndex = request.requestedFrameIndex,
            .attributeSelection = request.loadAllAvailableAttributes
                ? ::datacodec::AttributeSelectionMode::AllAvailable
                : request.attributeTargets.empty() ? ::datacodec::AttributeSelectionMode::None
                                                   : ::datacodec::AttributeSelectionMode::Explicit,
            .attributeTargets = request.attributeTargets,
            .configuration = {
                .controlParams = request.controlParams ? *request.controlParams : ::datacodec::MakeDefaultDecodeControlParams(),
                .source = request.configurationSource ? *request.configurationSource : ::datacodec::DataCodecDecodeConfigurationSource{},
                .language = request.language,
            },
            .runRecordSink = request.runRecordSink,
            .resources = request.resources,
            .stopToken = request.stopToken,
        },
        .sourceIdentity = request.inputSourceIdentity,
        .encodedInputCachePolicy = request.encodedInputCachePolicy,
    });
    state.decodedFramePackage = decoded.decodedFramePackage;
    if (decoded.success) {
        std::string error;
        if (!state.frameAssembly.Import(decoded.output, &error)) {
            decoded.success = false;
            decoded.failure = ::datacodec::MakeCodecFailureRecord(::datacodec::CodecErrorCode::DecodeFailure,
                "native-result", "DataCodecDataObjectBridge", error);
        } else { state.output = state.frameAssembly.Output(); }
    }
    auto result = ConvertPackageDecodeResult(std::move(decoded), state.output, request.runRecordSink.get());
    if (!result.success) state.Reset();
    return result;
}

DataCodecDataObjectDecodeResult DataCodecDataObjectDecodeSession::RequestAttributes(
    const DataCodecDataObjectAttributeRequest& request) {
    std::lock_guard lock(m_impl->mutex);
    auto& state = *m_impl;
    auto decoded = state.session.RequestAttributes({
        .targets = request.attributeTargets,
        .mode = request.mode,
        .runRecordSink = request.runRecordSink,
        .stopToken = request.stopToken,
    });
    if (decoded.success) {
        for (const auto& leaf : decoded.output.leaves) {
            std::string error;
            auto adapter = state.frameAssembly.CreateiGameSupplementAdapter(leaf.path, &error);
            if (!adapter || !adapter->Import(leaf, true, &error)) {
                decoded.success = false;
                decoded.failure = ::datacodec::MakeCodecFailureRecord(::datacodec::CodecErrorCode::DecodeFailure,
                    "native-attribute-result", "DataCodecDataObjectBridge", error);
                break;
            }
        }
    }
    auto result = ConvertPackageDecodeResult(std::move(decoded), state.output, request.runRecordSink.get());
    if (!result.success) state.Reset();
    return result;
}

std::vector<::datacodec::DecodeAttributeDescriptor> DataCodecDataObjectDecodeSession::AvailableAttributes() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->session.AvailableAttributes();
}

int DataCodecDataObjectDecodeSession::NativeAttributeIndex(const ::datacodec::AttributeTarget& target) const {
    std::lock_guard lock(m_impl->mutex);
    return iGameDecodeAdapter::NativeAttributeIndex(OutputForTarget(target), target.attrIndex);
}

DataObject::Pointer DataCodecDataObjectDecodeSession::OutputForTarget(const ::datacodec::AttributeTarget& target) const {
    std::lock_guard lock(m_impl->mutex);
    if (!m_impl->session.IsOpen()) return nullptr;
    return m_impl->decodedFramePackage ? m_impl->frameAssembly.LeafOutput(target.blockPath) : m_impl->output;
}

DataObject::Pointer DataCodecDataObjectDecodeSession::GetOutput() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->output;
}

bool DataCodecDataObjectDecodeSession::IsOpen() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->session.IsOpen();
}


::datacodec::EncodedInputCacheStats DataCodecDataObjectDecodeSession::InputCacheStatistics() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->session.InputCacheStatistics();
}

void DataCodecDataObjectDecodeSession::Reset() {
    if (!m_impl) { m_impl = std::make_unique<Impl>(); return; }
    std::lock_guard lock(m_impl->mutex);
    m_impl->Reset();
}

DataCodecDataObjectDecodeResult DecodeDataCodecDataObject(const DataCodecDataObjectDecodeRequest& request) {
    DataCodecDataObjectDecodeSession session;
    return session.Open(request);
}

IGAME_NAMESPACE_END
