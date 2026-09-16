#include "DataCodec/Storage/ByteIO/EncodedInputAccess.h"
#include "DataCodec/API/Entry/PackageDecodeSession.h"

#include "DataCodec/Workflow/Session/DecodeSession.h"
#include "DataCodec/Workflow/Session/CodecRunEntry.h"
#include "DataCodec/Runtime/Execution/DataCodecExecutionResources.h"
#include "DataCodec/Workflow/Decode/DecodedResultBuilder.h"

#include <map>
#include <tuple>

namespace datacodec {
namespace {

DecodePackageResult SessionFailure(std::string_view reason, std::string_view detail,
                                  bool cancelled = false) noexcept {
    DecodePackageResult result;
    result.cancelled = cancelled;
    result.failure = MakeCodecFailureRecord(CodecErrorCode::DecodeFailure,
        reason, "PackageDecodeSession", detail, cancelled);
    return result;
}

} // 匿名命名空间

struct PackageDecodeSession::Impl {
    using PreparedKey = std::tuple<std::uint32_t, BlockPath, std::vector<std::size_t>>;
    std::shared_ptr<DataCodecExecutionResources> resources;
    DecodeSession session;
    std::shared_ptr<IByteRangeReader> reader;
    DataCodecDecodePackageConfigurationParams configuration{MakeDefaultDecodePackageConfigurationParams()};
    std::map<PreparedKey, std::unique_ptr<DecodedLeafBuilder>> prepared;
    std::shared_ptr<const ICellTypeMapping> mapping;
    std::uint64_t inputBytes{0u};
    InputMemoryObservation inputMemory;
    bool framePackage{false};
    bool open{false};

    void Reset() noexcept {
        // 先清理解码状态，再释放其借用的适配器和资源根
        session.AbortFramePackage();
        prepared.clear();
        mapping.reset();
        reader.reset();
        resources.reset();
        inputBytes = 0u;
        inputMemory = {};
        framePackage = false;
        open = false;
    }
    ~Impl() { Reset(); }

    DecodePackageResult Finish(CodecRunScope& run, DecodePackageResult result) {
        if (result.failure) resources->RecordFailure(*result.failure);
        result.success = run.Finish(result.success);
        if (resources->FirstFailure()) result.failure = resources->FirstFailure();
        result.cancelled = result.cancelled || (result.failure && result.failure->cancelled);
        result.inputBytes = inputBytes;
        result.inputMemory = inputMemory;
        result.decodedFramePackage = framePackage;
        if (!result.success) { result.output = {}; }
        return result;
    }
};

PackageDecodeSession::PackageDecodeSession() : m_impl(std::make_unique<Impl>()) {}
PackageDecodeSession::~PackageDecodeSession() = default;

DecodePackageResult PackageDecodeSession::Open(const PackageDecodeSessionOpenRequest& request) {
    Reset();
    auto& state = *m_impl;
    DecodePackageResult result;
    try {
        if (request.decode.stopToken.stop_requested()) {
            return SessionFailure("cancelled", "decode session cancelled", true);
        }
        if (!request.decode.input) {
            return SessionFailure("missing-input", "decode session requires an input reader");
        }
        state.reader = EncodedInputAccess::Open(request.decode.input);
        state.inputMemory = request.decode.input.ObserveMemory();
        state.inputBytes = state.reader->ByteSize();
        state.resources = std::make_shared<DataCodecExecutionResources>(request.decode.resources);
        const auto resources = state.resources;
        {
            CodecRunScope run(*resources);
            std::stop_callback stop(request.decode.stopToken, [resources] { resources->RequestStop(); });
            if (!run) {
                result = SessionFailure("run-start", "decode session could not start");
            } else {
                auto decode = request.decode;

                state.configuration = decode.configuration;
                state.mapping = decode.cellTypeMapping;
                if (request.encodedInputCachePolicy.enabled && request.sourceIdentity.IsStable()) {
                    auto& caches = resources->Caches();
                    std::string error;
                    state.reader = caches.EncodedInputLoader().Load(*resources,
                        caches.DefaultEncodedInputCache(), request.sourceIdentity,
                        state.reader, EncodedInputAccessKind::UserRequest, &error);
                    if (!state.reader) {
                        result = SessionFailure("input-cache", error.empty()
                            ? "decode session failed to retain encoded input" : error);
                    }
                }
                if (state.reader) {
                    decode.input = EncodedInputAccess::Retain(state.reader);
                    result = DecodePackageInRun(decode, *resources, &state.session);
                    state.framePackage = result.decodedFramePackage;
                }
                result = state.Finish(run, std::move(result));
            }
        }
        state.open = result.success;
        if (!result.success) state.Reset();
        return result;
    } catch (const std::bad_alloc&) {
        result = SessionFailure("allocation-failed", "memory allocation failed");
    } catch (const std::exception& error) {
        result = SessionFailure("session-exception", error.what());
    } catch (...) {
        result = SessionFailure("session-exception", "unknown exception");
    }
    result.inputBytes = state.inputBytes;
    state.Reset();
    return result;
}

DecodePackageResult PackageDecodeSession::RequestAttributes(
    const PackageDecodeSessionAttributeRequest& request) {
    auto& state = *m_impl;
    if (!state.open) return SessionFailure("session-closed", "decode session is not open");
    if (request.targets.empty()) return {.success = true, .decodedFramePackage = state.framePackage,
                                         .inputBytes = state.inputBytes, .inputMemory = state.inputMemory};
    DecodePackageResult result;
    try {
        using TargetKey = std::pair<std::uint32_t, BlockPath>;
        std::map<TargetKey, std::vector<AttributeTarget>> grouped;
        const auto frame = state.session.OutputFrameIndex();
        for (const auto& target : request.targets) {
            if (!frame || target.frameIndex != *frame) {
                return SessionFailure("attribute-target", "attribute target does not belong to the session output frame");
            }
            grouped[{target.frameIndex, target.blockPath}].push_back(target);
        }
        if (request.mode != AttributeDecodeRequestMode::DecodeAndCommit &&
            request.mode != AttributeDecodeRequestMode::DecodeToCache &&
            request.mode != AttributeDecodeRequestMode::CommitCached) {
            return SessionFailure("attribute-mode", "unknown attribute request mode");
        }
        const auto resources = state.resources;
        {
            CodecRunScope run(*resources);
            std::stop_callback stop(request.stopToken, [resources] { resources->RequestStop(); });
            result.success = static_cast<bool>(run);
            if (!run) result = SessionFailure("run-start", "attribute request could not start");
            for (const auto& [target, targets] : grouped) {
                if (!result.success) break;
                if (request.stopToken.stop_requested()) {
                    result = SessionFailure("cancelled", "attribute request cancelled", true);
                    break;
                }
                std::vector<std::size_t> indices;
                for (const auto& entry : targets) indices.push_back(entry.attrIndex);
                std::sort(indices.begin(), indices.end());
                indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
                const Impl::PreparedKey key{target.first, target.second, std::move(indices)};
                auto prepared = state.prepared.find(key);
                if (prepared == state.prepared.end()) {
                    if (request.mode == AttributeDecodeRequestMode::CommitCached) {
                        result = SessionFailure("attribute-cache", "prepared attribute adapter is unavailable");
                        break;
                    }
                    auto adapter = std::make_unique<DecodedLeafBuilder>(state.mapping);
                    adapter->output.path = target.second;
                    prepared = state.prepared.emplace(key, std::move(adapter)).first;
                }
                auto& adapter = *prepared->second;
                auto leaf = state.session.SupplementLeafAttributes({
                    .adapter = &adapter,
                    .leafPackage = nullptr,
                    .frameIndex = target.first,
                    .attributeSelection = AttributeSelectionMode::Explicit,
                    .attributeTargets = std::span<const AttributeTarget>(targets),
                    .supplementAttributesOnly = true,
                    .attributeRequestMode = request.mode,
                    .controlParams = state.configuration.controlParams,
                    .configurationSource = state.configuration.source,
                    .language = state.configuration.language,
                    .runRecordSink = request.runRecordSink.get(),
                    .stopToken = request.stopToken,
                    .resources = resources.get(),
                });
                AppendRetainedTelemetryMessages(result.messages, leaf.messages);
                result.success = leaf.success;
                result.failure = leaf.failure;
                if (!result.success) break;
                if (request.mode != AttributeDecodeRequestMode::DecodeToCache) {
                    result.output.frameIndex = target.first;
                    result.output.leaves.push_back(std::move(adapter.output));
                    state.prepared.erase(prepared);
                }
            }
            result = state.Finish(run, std::move(result));
        }
        if (!result.success) state.Reset();
        return result;
    } catch (const std::bad_alloc&) {
        result = SessionFailure("allocation-failed", "memory allocation failed");
    } catch (const std::exception& error) {
        result = SessionFailure("session-exception", error.what());
    } catch (...) {
        result = SessionFailure("session-exception", "unknown exception");
    }
    result.inputBytes = state.inputBytes;
    state.Reset();
    return result;
}

std::vector<DecodeAttributeDescriptor> PackageDecodeSession::AvailableAttributes() const {
    if (!m_impl->open) return {};
    auto result = m_impl->session.AvailableAttributes();
    const auto frame = m_impl->session.OutputFrameIndex();
    std::erase_if(result, [&frame](const auto& entry) { return !frame || entry.target.frameIndex != *frame; });
    return result;
}

EncodedInputCacheStats PackageDecodeSession::InputCacheStatistics() const {
    return m_impl->resources ? m_impl->resources->Caches().DefaultEncodedInputCache()->Statistics() : EncodedInputCacheStats{};
}
bool PackageDecodeSession::IsOpen() const noexcept { return m_impl->open; }
void PackageDecodeSession::Reset() noexcept { m_impl->Reset(); }

} // 命名空间 datacodec
