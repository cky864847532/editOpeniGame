#include "DataCodec/Filter/Adapter/iGameDataCodecDataObjectBridge.h"
#include "DataCodec/Filter/Adapter/iGameDecodeAdapter.h"
#include "DataCodec/Filter/Adapter/iGameFramePackageDecodeAssembly.h"
#include "DataCodec/Runtime/Record/RunRecordSubmit.h"
#include "DataCodec/Validation/Filter/FilterCommitValidator.h"
#include "DataCodec/Validation/Workflow/DecodeValidationLifecycle.h"

#include <map>
#include <mutex>
#include <tuple>

IGAME_NAMESPACE_BEGIN
namespace {

DataCodecDataObjectDecodeResult ConvertPackageDecodeResult(
    ::datacodec::DecodePackageResult decoded, DataObject::Pointer output,
    ::datacodec::IRunRecordSink* sink) {
    DataCodecDataObjectDecodeResult result;
    result.success = decoded.success && output != nullptr;
    result.output = std::move(output);
    result.inputBytes = decoded.inputBytes;
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
    using NativeAttributeKey = std::tuple<std::uint32_t, ::datacodec::BlockPath, std::size_t>;
    mutable std::recursive_mutex mutex;
    iGameDecodeAdapter initialLeafAdapter;
    iGameFramePackageDecodeAssembly frameAssembly;
    // 会话析构早于其借用的两个适配器
    ::datacodec::PackageDecodeSession session;
    DataObject::Pointer output;
    std::map<NativeAttributeKey, int> nativeAttributeIndices;
    bool decodedFramePackage{false};

    void Reset() noexcept {
        session.Reset();
        initialLeafAdapter.ResetOutput();
        frameAssembly.AbortFramePackage();
        output = nullptr;
        nativeAttributeIndices.clear();
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
            .inputReader = request.inputReader,
            .leafAdapter = &state.initialLeafAdapter,
            .frameAssembly = &state.frameAssembly,
            .requestedFrameIndex = request.requestedFrameIndex,
            .attributeSelection = request.loadAllAvailableAttributes
                ? ::datacodec::AttributeSelectionMode::AllAvailable
                : request.attributeTargets.empty() ? ::datacodec::AttributeSelectionMode::None
                                                   : ::datacodec::AttributeSelectionMode::Explicit,
            .attributeTargets = request.attributeTargets,
            .configuration = {
                .controlParams = request.controlParams ? *request.controlParams : ::datacodec::MakeDefaultDecodeControlParams(),
                .execution = request.executionOptions ? *request.executionOptions : ::datacodec::MakeDefaultDecodeExecutionOptions(),
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
        state.output = decoded.decodedFramePackage ? state.frameAssembly.Output() : state.initialLeafAdapter.TakeDataObject();
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
        .createAdapter = [&state](const ::datacodec::BlockPath& path, std::string* error)
            -> std::unique_ptr<::datacodec::IDecodeAdapter> {
            if (state.decodedFramePackage) return state.frameAssembly.CreateSupplementAdapter(path, error);
            return std::make_unique<iGameDecodeAdapter>(state.output);
        },
        .afterDecode = [&state](::datacodec::IDecodeAdapter& adapter,
                               std::span<const ::datacodec::AttributeTarget> targets) {
            const auto& native = static_cast<const iGameDecodeAdapter&>(adapter);
            for (const auto& target : targets) {
                const auto index = native.NativeAttributeIndex(target.attrIndex);
                if (index >= 0) state.nativeAttributeIndices[{target.frameIndex, target.blockPath, target.attrIndex}] = index;
            }
        },
    });
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
    const auto found = m_impl->nativeAttributeIndices.find({target.frameIndex, target.blockPath, target.attrIndex});
    return found == m_impl->nativeAttributeIndices.end() ? -1 : found->second;
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

::datacodec::DecodedFrameCacheStats DataCodecDataObjectDecodeSession::DecodedCacheStatistics() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->session.DecodedCacheStatistics();
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
