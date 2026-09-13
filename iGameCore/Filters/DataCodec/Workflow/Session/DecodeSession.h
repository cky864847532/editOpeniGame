#ifndef DATACODEC_WORKFLOW_SESSION_DECODESESSION_H
#define DATACODEC_WORKFLOW_SESSION_DECODESESSION_H

#include "DataCodec/API/Adapter/IFramePackageDecodeAssembly.h"
#include "DataCodec/API/Adapter/DecodedFrameTypes.h"
#include "DataCodec/Storage/FramePackage/FramePackageFormat.h"
#include "DataCodec/Workflow/Leaf/LeafDecodeExecutor.h"
#include "DataCodec/Workflow/Session/DataCodecReferenceState.h"
#include "DataCodec/Runtime/Cache/DecodeReferenceCache.h"
#include "DataCodec/Runtime/Cache/DecodeCacheRuntime.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "DataCodec/Workflow/Decode/DecodeStoragePlan.h"

namespace datacodec {

class DecodeSession {
public:
    using FrameIdentityMap = std::unordered_map<std::uint32_t, DecodeSourceIdentity>;

    ~DecodeSession() {
        if (m_frameBegun || m_frameAssembly != nullptr) {
            AbortFramePackage();
        } else {
            ReleaseSessionStateNoThrow();
        }
    }

    void ConfigureReferences(
        DataCodecExecutionResources& resources,
        std::shared_ptr<const FrameIdentityMap> frameIdentities) {
        m_referenceCache = resources.Caches().ReferenceCache();
        m_frameIdentities = std::move(frameIdentities);
    }

    // 由串行 driver 在无在途任务时分析补充请求，容量快照独立于占用审计
    [[nodiscard]] DecodeStorageAnalysisResult AnalyzeAttributeStorage(
        const DecodeStorageAnalysisRequest& request, DataCodecExecutionResources& resources) const {
        ResourceDebugSnapshot snapshot;
        if (!resources.TryCopyResourceDebugSnapshot(snapshot) || snapshot.admittedBlocks != 0u ||
            snapshot.activeComputeUnits != 0u) {
            DecodeStorageAnalysisResult result;
            result.failure = MakeCodecFailureRecord(CodecErrorCode::PipelineFailure, "decode-storage-analysis",
                "DecodeSession", "attribute storage analysis requires a drained request");
            return result;
        }
        storageplan::ExistingState existing;
        const auto idle = resources.Scratch().RetainedFixedBytes();
        existing.reservedBytes = snapshot.storage.reservedBytes - std::min(snapshot.storage.reservedBytes, idle);
        existing.supplementAttributesOnly = true;
        auto append = [&](std::uint32_t frameIndex, const BlockPath& path, const LeafPackage* package,
                          const DecodedAttributeCacheSet* attributes, bool geometryReady, bool topologyReady) {
            for (const auto& leaf : existing.leaves) {
                if (leaf.frameIndex == frameIndex && leaf.package.path == path) { return; }
            }
            storageplan::ExistingLeaf leaf;
            leaf.frameIndex = frameIndex;
            if (package) { leaf.package = *package; }
            leaf.package.path = path;
            leaf.geometryReferenceReady = geometryReady;
            leaf.topologyReady = topologyReady;
            if (attributes && attributes->IsInitialized()) {
                for (std::size_t i = 0u; i < attributes->FieldCount(); ++i) {
                    leaf.completeAttributes.push_back(attributes->Complete(i));
                    leaf.adapterBackedAttributes.push_back(attributes->AdapterBacked(i));
                }
            }
            existing.leaves.push_back(std::move(leaf));
        };
        for (const auto& [key, state] : m_leafStates) {
            const auto& workspace = *state->workspace;
            append(state->frameIndex, state->leafPackage.path, &state->leafPackage, &workspace.attributes,
                state->geometryReferenceCache && state->geometryReferenceCache->IsComplete(),
                workspace.topology && workspace.topology->complete);
        }
        // reference 的属性、拓扑和 workspace 可以共享 owner，全部由根预约基线计一次
        if (m_frameIdentities) {
            for (const auto& [frameIndex, identity] : *m_frameIdentities) {
                const auto cached = FindReferenceFrame(frameIndex);
                // 按需补充允许 reference 中仅有部分属性完成，逐字段记录当前状态
                if (!cached) { continue; }
                for (const auto& [path, leaf] : cached->leaves) {
                    const auto* package = leaf.attribute ? &leaf.attribute->reference.leafPackage :
                        leaf.geometry ? &leaf.geometry->reference.leafPackage : nullptr;
                    append(frameIndex, path, package, leaf.attribute ? leaf.attribute->store.get() : nullptr,
                        leaf.geometry && leaf.geometry->store && leaf.geometry->store->IsComplete(), !leaf.topology.empty());
                }
            }
        }
        return storageplan::Analyze(request, existing);
    }

    bool BeginFramePackage(
        const FramePackage& framePackage,
        IFramePackageDecodeAssembly& frameAssembly,
        const std::optional<std::uint32_t> requestedFrameIndex,
        std::string* error = nullptr) {
        if (m_frameBegun || m_frameAssembly != nullptr) {
            AbortFramePackage();
        } else {
            ResetFrameAssembly();
        }
        if (requestedFrameIndex.has_value() && *requestedFrameIndex != framePackage.frameIndex) {
            AssignError(error, "requested frame index is not available in this frame package");
            return false;
        }
        if (!frameAssembly.BeginFramePackage(framePackage, error)) {
            AbortAssemblyNoThrow(&frameAssembly);
            AssignDefaultError(error, "failed to begin frame package assembly");
            return false;
        }

        m_frameAssembly = &frameAssembly;
        m_frameBegun = true;
        m_activeFramePackage = ActiveFrameMetadata{
            framePackage.frameIndex, framePackage.geometryTemporalRole, framePackage.geometryKeyFrameIndex,
            framePackage.attributeTemporalRole, framePackage.attributeKeyFrameIndex};
        m_outputFrameIndex = framePackage.frameIndex;

        std::vector<const FramePackageBranchRecord*> branches;
        branches.reserve(framePackage.branches.size());
        for (const auto& branch : framePackage.branches) { branches.push_back(&branch); }
        std::sort(
            branches.begin(),
            branches.end(),
            [](const auto& left, const auto& right) {
                return std::count(left->path.begin(), left->path.end(), '/') <
                    std::count(right->path.begin(), right->path.end(), '/');
        });
        for (const auto& branch : branches) {
            if (!frameAssembly.AddBranch(*branch, error)) {
                AssignDefaultError(error, "failed to add frame package branch");
                AbortFramePackage();
                return false;
            }
        }
        return true;
    }

    std::unique_ptr<IDecodeAdapter> CreateLeafAdapter(
        const FramePackageLeafRecord& leaf,
        const LeafPackage& leafPackage,
        std::string* error = nullptr) {
        if (m_frameAssembly == nullptr || !m_frameBegun) {
            AssignError(error, "DataCodec decode workspace has no active frame assembly");
            return nullptr;
        }
        return m_frameAssembly->CreateLeafAdapter(leaf, leafPackage, error);
    }

    LeafDecodeResult DecodeLeaf(const LeafDecodeRequest& request) {
        if (request.leafPackage == nullptr) {
            return LeafDecodeExecutor::Execute(request);
        }
        const auto key = MakeLeafKey(request.frameIndex, request.leafPackage->path);
        m_outputFrameIndex = request.frameIndex;
        auto state = std::make_unique<LeafDecodeState>();
        state->frameIndex = request.frameIndex;
        state->leafPackage = *request.leafPackage;
        state->workspace = std::make_shared<DecodeLeafWorkspace>();
        state->attributeKeyFrameIndex = m_activeFramePackage.has_value()
            ? m_activeFramePackage->attributeKeyFrameIndex
            : request.frameIndex;
        state->geometryKeyFrameIndex = m_activeFramePackage.has_value()
            ? m_activeFramePackage->geometryKeyFrameIndex
            : request.frameIndex;
        state->topologyReferenceKey = request.topologyReferenceKey;
        state->topologyOwnerFrameIndex = request.topologyOwnerFrameIndex;
        if (m_activeFramePackage.has_value() &&
            m_activeFramePackage->geometryTemporalRole != TemporalFieldRole::SingleFrame &&
            state->frameIndex == state->geometryKeyFrameIndex) {
            state->geometryReferenceCache = std::make_shared<DecodedGeometryReferenceCache>();
        }
        auto* statePointer = state.get();
        m_leafStates[key] = std::move(state);
        if (m_state.decodeTopologyReferenceStore == nullptr) {
            m_state.decodeTopologyReferenceStore = std::make_shared<DecodedTopologyReferenceCacheStore>();
        }
        DecodedAttributeReference* attributeReference = nullptr;
        DecodedGeometryReference* geometryReference = nullptr;
        LoadCachedReferences(*statePointer, attributeReference, geometryReference);
        auto leafRequest = request;
        leafRequest.leafPackage = &statePointer->leafPackage;
        leafRequest.attributeKeyFrameReference = attributeReference;
        leafRequest.geometryKeyFrameReference = geometryReference;
        leafRequest.currentGeometryReferenceCache = statePointer->geometryReferenceCache.get();
        leafRequest.topologyReferenceStore = m_state.decodeTopologyReferenceStore.get();
        leafRequest.workspace = statePointer->workspace.get();
        auto result = LeafDecodeExecutor::Execute(leafRequest);
        const bool executorSucceeded = result.success;
        if (result.success && m_activeFramePackage.has_value()) {
            if (m_activeFramePackage->attributeTemporalRole != TemporalFieldRole::SingleFrame &&
                statePointer->frameIndex == statePointer->attributeKeyFrameIndex) {
                std::string referenceError;
                if (!statePointer->workspace->attributes.IsInitialized() &&
                    !statePointer->workspace->attributes.Initialize(
                        statePointer->workspace->StorageParams(),
                        statePointer->workspace->ByteStoreSessionRef(),
                        &referenceError)) {
                    result.success = false;
                    result.failure = MakeCodecFailureRecord(
                        CodecErrorCode::DecodeFailure, "operation-failed", "DecodeSession",
                        referenceError.empty()
                            ? "failed to initialize lazy attribute reference store"
                            : std::string_view(referenceError));
                }
            }
            if (result.success &&
                m_activeFramePackage->attributeTemporalRole != TemporalFieldRole::SingleFrame &&
                statePointer->frameIndex == statePointer->attributeKeyFrameIndex) {
                statePointer->attributeReference = DecodedAttributeReference{
                    .reference = AttributeReference{
                        .storageParams = statePointer->workspace->StorageParams(),
                        .leafPackage = statePointer->leafPackage,
                    },
                    .store = std::shared_ptr<DecodedAttributeCacheSet>(
                        statePointer->workspace,
                        &statePointer->workspace->attributes),
                    .byteStoreSession = std::shared_ptr<bytestore::ByteStoreSession>(
                        statePointer->workspace,
                        &statePointer->workspace->ByteStoreSessionRef()),
                };
            }
            if (statePointer->geometryReferenceCache != nullptr &&
                statePointer->geometryReferenceCache->IsComplete()) {
                statePointer->geometryReference = DecodedGeometryReference{
                    .reference = GeometryReference{
                        .storageParams = statePointer->workspace->StorageParams(),
                        .leafPackage = statePointer->leafPackage,
                    },
                    .store = statePointer->geometryReferenceCache,
                    .byteStoreSession = std::shared_ptr<bytestore::ByteStoreSession>(
                        statePointer->workspace,
                        &statePointer->workspace->ByteStoreSessionRef()),
                };
            }
        }
        statePointer->attributeReferenceOwner.reset();
        statePointer->geometryReferenceOwner.reset();
        statePointer->topologyReferenceOwner.reset();
        if (!result.success) {
            if (executorSucceeded) {
                CleanupFailedLeaf(*statePointer, request.adapter);
            }
            m_leafStates.erase(key);
        }
        return result;
    }

    LeafDecodeResult SupplementLeafAttributes(const LeafDecodeRequest& request) {
        const auto path = request.leafPackage != nullptr
            ? request.leafPackage->path
            : !request.attributeTargets.empty() ? request.attributeTargets.front().blockPath : BlockPath{};
        const auto key = MakeLeafKey(request.frameIndex, path);
        const auto iterator = m_leafStates.find(key);
        if (iterator == m_leafStates.end()) {
            auto leafRequest = request;
            leafRequest.supplementAttributesOnly = true;
            return LeafDecodeExecutor::Execute(leafRequest);
        }
        auto& state = *iterator->second;
        DecodedAttributeReference* attributeReference = nullptr;
        DecodedGeometryReference* unusedGeometryReference = nullptr;
        LoadCachedReferences(state, attributeReference, unusedGeometryReference);
        auto leafRequest = request;
        leafRequest.leafPackage = &state.leafPackage;
        leafRequest.attributeKeyFrameReference = attributeReference;
        leafRequest.topologyReferenceStore = m_state.decodeTopologyReferenceStore.get();
        leafRequest.workspace = state.workspace.get();
        leafRequest.supplementAttributesOnly = true;
        auto result = LeafDecodeExecutor::Execute(leafRequest);
        const bool executorSucceeded = result.success;
        if (result.success && state.attributeReference.has_value() &&
            state.attributeReference->store != nullptr &&
            state.attributeReference->store->IsComplete()) {
            std::string publishError;
            if (!PublishCurrentFrameReferences(&publishError)) {
                result.success = false;
                result.failure = MakeCodecFailureRecord(
                    CodecErrorCode::DecodeFailure, "operation-failed", "DecodeSession",
                    publishError.empty()
                        ? "failed to publish supplemented decode reference"
                        : std::string_view(publishError));
            }
        }
        state.attributeReferenceOwner.reset();
        state.geometryReferenceOwner.reset();
        state.topologyReferenceOwner.reset();
        if (!result.success) {
            if (executorSucceeded) {
                CleanupFailedLeaf(state, request.adapter);
            }
            m_leafStates.erase(iterator);
        }
        return result;
    }

    [[nodiscard]] std::vector<AttributeTarget> AvailableAttributeTargets(
        const std::uint32_t frameIndex,
        const BlockPath& path) const {
        std::vector<AttributeTarget> targets;
        const auto iterator = m_leafStates.find(MakeLeafKey(frameIndex, path));
        if (iterator == m_leafStates.end()) {
            return targets;
        }
        const auto attrCount = iterator->second->workspace->StorageParams().attrParams.size();
        targets.reserve(attrCount);
        for (std::size_t attrIndex = 0u; attrIndex < attrCount; ++attrIndex) {
            targets.push_back(AttributeTarget{
                .frameIndex = frameIndex,
                .blockPath = path,
                .attrIndex = attrIndex,
            });
        }
        return targets;
    }

    [[nodiscard]] std::vector<DecodeAttributeDescriptor> AvailableAttributes() const {
        std::vector<DecodeAttributeDescriptor> descriptors;
        for (const auto& [key, stateOwner] : m_leafStates) {
            (void)key;
            const auto& state = *stateOwner;
            const auto& attrParams = state.workspace->StorageParams().attrParams;
            descriptors.reserve(descriptors.size() + attrParams.size());
            for (std::size_t attrIndex = 0u; attrIndex < attrParams.size(); ++attrIndex) {
                descriptors.push_back(DecodeAttributeDescriptor{
                    .target = AttributeTarget{
                        .frameIndex = state.frameIndex,
                        .blockPath = state.leafPackage.path,
                        .attrIndex = attrIndex,
                    },
                    .metadata = attrParams[attrIndex],
                    .decoded = state.workspace->attributes.Complete(attrIndex),
                    .committed = state.workspace->AttributeCommitted(attrIndex),
                });
            }
        }
        std::sort(
            descriptors.begin(),
            descriptors.end(),
            [](const DecodeAttributeDescriptor& left, const DecodeAttributeDescriptor& right) {
                if (left.target.frameIndex != right.target.frameIndex) {
                    return left.target.frameIndex < right.target.frameIndex;
                }
                if (left.target.blockPath != right.target.blockPath) {
                    return left.target.blockPath < right.target.blockPath;
                }
                return left.target.attrIndex < right.target.attrIndex;
            });
        return descriptors;
    }

    [[nodiscard]] std::optional<std::uint32_t> OutputFrameIndex() const noexcept {
        return m_outputFrameIndex;
    }

    [[nodiscard]] std::uint64_t ResidentSizeHint() const noexcept {
        std::uint64_t bytes = 0u;
        for (const auto& [key, stateOwner] : m_leafStates) {
            (void)key;
            const auto& state = *stateOwner;
            const auto storeStats = state.workspace->ByteStoreSessionRef().SnapshotStats();
            bytes = validation::SaturatingAddU64(bytes, storeStats.residentBytes);
            bytes = validation::SaturatingAddU64(
                bytes,
                state.workspace->attributes.AdapterBackedResidentSizeHint());
        }
        return bytes;
    }

    [[nodiscard]] std::size_t LeafStateCount() const noexcept {
        return m_leafStates.size();
    }

    [[nodiscard]] std::vector<std::uint32_t> RetainedFrameIndices() const {
        std::vector<std::uint32_t> frames;
        frames.reserve(m_leafStates.size());
        for (const auto& [key, state] : m_leafStates) {
            (void)key;
            frames.push_back(state->frameIndex);
        }
        std::sort(frames.begin(), frames.end());
        frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
        return frames;
    }

    bool CommitLeaf(
        const FramePackageLeafRecord& leaf,
        IDecodeAdapter& adapter,
        std::string* error = nullptr) {
        if (m_frameAssembly == nullptr || !m_frameBegun) {
            AssignError(error, "DataCodec decode workspace has no active frame assembly");
            return false;
        }
        return m_frameAssembly->CommitLeaf(leaf, adapter, error);
    }

    bool EndFramePackage(std::string* error = nullptr) {
        if (m_frameAssembly == nullptr || !m_frameBegun) {
            AssignError(error, "DataCodec decode workspace has no active frame assembly");
            return false;
        }
        auto* assembly = m_frameAssembly;
        try {
            if (!assembly->EndFramePackage(error)) {
                AssignDefaultError(error, "failed to end frame package assembly");
                AbortFramePackage();
                return false;
            }
            if (!PublishCurrentFrameReferences(error)) {
                AbortFramePackage();
                return false;
            }
        } catch (const std::exception& exception) {
            AbortFramePackage();
            AssignError(
                error,
                std::string("frame package finalization failed: ") + exception.what());
            return false;
        } catch (...) {
            AbortFramePackage();
            AssignError(error, "frame package finalization failed");
            return false;
        }
        PruneCompletedFrameStates();
        ResetFrameAssembly();
        return true;
    }

    void AbortFramePackage() noexcept {
        AbortAssemblyNoThrow(m_frameAssembly);
        CleanupLeafStatesNoThrow();
        ReleaseSessionStateNoThrow();
    }

    DataCodecReferenceState& ReferenceState() noexcept { return m_state; }
    const DataCodecReferenceState& ReferenceState() const noexcept { return m_state; }

private:
    struct LeafDecodeState {
        std::uint32_t frameIndex{0u};
        LeafPackage leafPackage;
        std::shared_ptr<DecodeLeafWorkspace> workspace;
        std::shared_ptr<DecodedGeometryReferenceCache> geometryReferenceCache;
        std::uint32_t attributeKeyFrameIndex{0u};
        std::uint32_t geometryKeyFrameIndex{0u};
        std::string topologyReferenceKey;
        std::uint32_t topologyOwnerFrameIndex{0u};
        std::optional<DecodedAttributeReference> attributeReference;
        std::optional<DecodedGeometryReference> geometryReference;
        DecodeReferenceCache::FramePointer attributeReferenceOwner;
        DecodeReferenceCache::FramePointer geometryReferenceOwner;
        DecodeReferenceCache::FramePointer topologyReferenceOwner;
    };

    [[nodiscard]] static std::string MakeLeafKey(
        const std::uint32_t frameIndex,
        const BlockPath& path) {
        return std::to_string(frameIndex) + "\n" + path;
    }

    static void AssignError(std::string* error, std::string text) {
        if (error != nullptr) {
            *error = std::move(text);
        }
    }

    static void AssignDefaultError(std::string* error, std::string text) {
        if (error != nullptr && error->empty()) {
            *error = std::move(text);
        }
    }

    static void AbortAssemblyNoThrow(IFramePackageDecodeAssembly* assembly) noexcept {
        if (assembly == nullptr) {
            return;
        }
        try {
            assembly->AbortFramePackage();
        } catch (...) {
        }
    }

    void ReleaseSessionStateNoThrow() noexcept {
        ResetFrameAssembly();
        m_state.ResetDecodeReferences();
        m_leafStates.clear();
        m_activeFramePackage.reset();
        m_outputFrameIndex.reset();
    }

    static void CleanupFailedLeaf(
        LeafDecodeState& state,
        IDecodeAdapter* adapter) noexcept {
        if (state.workspace != nullptr) {
            state.workspace->CleanupOnFailure();
        }
        if (adapter != nullptr) {
            try {
                adapter->Abort();
            } catch (...) {
            }
        }
    }

    void CleanupLeafStatesNoThrow() noexcept {
        for (auto& [key, stateOwner] : m_leafStates) {
            (void)key;
            if (stateOwner == nullptr || stateOwner->workspace == nullptr) {
                continue;
            }
            stateOwner->workspace->CleanupOnFailure();
        }
    }

    [[nodiscard]] std::optional<DecodeReferenceKey> ReferenceKeyForFrame(
        const std::uint32_t frameIndex) const {
        if (!m_frameIdentities) { return std::nullopt; }
        const auto identity = m_frameIdentities->find(frameIndex);
        if (identity == m_frameIdentities->end() || !identity->second.IsStable()) {
            return std::nullopt;
        }
        return DecodeReferenceKey{
            .source = identity->second,
            .keyFrameIndex = frameIndex,
        };
    }

    [[nodiscard]] DecodeReferenceCache::FramePointer FindReferenceFrame(
        const std::uint32_t frameIndex) const {
        if (m_referenceCache == nullptr) { return nullptr; }
        const auto key = ReferenceKeyForFrame(frameIndex);
        return key.has_value() ? m_referenceCache->Find(*key) : nullptr;
    }

    void LoadCachedReferences(
        LeafDecodeState& state,
        DecodedAttributeReference*& attributeReference,
        DecodedGeometryReference*& geometryReference) {
        if (!m_activeFramePackage.has_value()) { return; }

        if (m_activeFramePackage->attributeTemporalRole == TemporalFieldRole::PredFrame) {
            state.attributeReferenceOwner = FindReferenceFrame(state.attributeKeyFrameIndex);
            if (state.attributeReferenceOwner != nullptr) {
                const auto leaf = state.attributeReferenceOwner->leaves.find(state.leafPackage.path);
                if (leaf != state.attributeReferenceOwner->leaves.end() && leaf->second.attribute.has_value()) {
                    attributeReference = &leaf->second.attribute.value();
                }
            }
        }

        if (m_activeFramePackage->geometryTemporalRole == TemporalFieldRole::PredFrame) {
            state.geometryReferenceOwner = FindReferenceFrame(state.geometryKeyFrameIndex);
            if (state.geometryReferenceOwner != nullptr) {
                const auto leaf = state.geometryReferenceOwner->leaves.find(state.leafPackage.path);
                if (leaf != state.geometryReferenceOwner->leaves.end() && leaf->second.geometry.has_value()) {
                    geometryReference = &leaf->second.geometry.value();
                }
            }
        }

        if (state.topologyOwnerFrameIndex != state.frameIndex && !state.topologyReferenceKey.empty()) {
            state.topologyReferenceOwner = FindReferenceFrame(state.topologyOwnerFrameIndex);
            if (state.topologyReferenceOwner != nullptr) {
                const auto leaf = state.topologyReferenceOwner->leaves.find(state.leafPackage.path);
                if (leaf != state.topologyReferenceOwner->leaves.end()) {
                    const auto topology = leaf->second.topology.find(state.topologyReferenceKey);
                    if (topology != leaf->second.topology.end()) {
                        if (m_state.decodeTopologyReferenceStore == nullptr) {
                            m_state.decodeTopologyReferenceStore =
                                std::make_shared<DecodedTopologyReferenceCacheStore>();
                        }
                        m_state.decodeTopologyReferenceStore->Put(
                            state.topologyReferenceKey,
                            topology->second.store);
                    }
                }
            }
        }
    }

    bool PublishCurrentFrameReferences(std::string* error = nullptr) {
        if (m_referenceCache == nullptr || !m_activeFramePackage.has_value()) { return true; }
        const auto frameIndex = m_activeFramePackage->frameIndex;
        const auto key = ReferenceKeyForFrame(frameIndex);
        if (!key.has_value()) {
            return true;
        }

        DecodeReferenceFrame frame;
        frame.frameIndex = frameIndex;
        bool ownsReference = false;
        for (const auto& [stateKey, stateOwner] : m_leafStates) {
            (void)stateKey;
            const auto& state = *stateOwner;
            if (state.frameIndex != frameIndex || state.workspace == nullptr) { continue; }
            auto& leaf = frame.leaves[state.leafPackage.path];

            if (m_activeFramePackage->attributeTemporalRole != TemporalFieldRole::SingleFrame &&
                frameIndex == state.attributeKeyFrameIndex) {
                ownsReference = true;
                leaf.requiresAttribute = true;
                if (state.attributeReference.has_value() &&
                    state.attributeReference->store != nullptr &&
                    state.attributeReference->store->IsInitialized()) {
                    leaf.attribute = state.attributeReference;
                } else {
                    return validation::AssignError(
                        error,
                        "decoded attribute reference is incomplete for leaf: " + state.leafPackage.path);
                }
            }

            if (m_activeFramePackage->geometryTemporalRole != TemporalFieldRole::SingleFrame &&
                frameIndex == state.geometryKeyFrameIndex) {
                ownsReference = true;
                leaf.requiresGeometry = true;
                if (state.geometryReference.has_value() &&
                    state.geometryReference->store != nullptr &&
                    state.geometryReference->store->IsComplete()) {
                    leaf.geometry = state.geometryReference;
                } else {
                    return validation::AssignError(
                        error,
                        "decoded geometry reference is incomplete for leaf: " + state.leafPackage.path);
                }
            }

            if (state.topologyOwnerFrameIndex == frameIndex && !state.topologyReferenceKey.empty()) {
                ownsReference = true;
                leaf.requiredTopology.insert(state.topologyReferenceKey);
                const auto topology = m_state.decodeTopologyReferenceStore != nullptr
                    ? m_state.decodeTopologyReferenceStore->Get(state.topologyReferenceKey)
                    : nullptr;
                if (topology != nullptr && topology->complete) {
                    leaf.topology.emplace(
                        state.topologyReferenceKey,
                        DecodedTopologyReference{
                            .store = topology,
                            .byteStoreSession = std::shared_ptr<bytestore::ByteStoreSession>(
                                state.workspace,
                                &state.workspace->ByteStoreSessionRef()),
                        });
                } else {
                    return validation::AssignError(
                        error,
                        "decoded topology reference is incomplete for leaf: " + state.leafPackage.path);
                }
            }

            if (!leaf.attribute.has_value() && !leaf.geometry.has_value() && leaf.topology.empty()) {
                frame.leaves.erase(state.leafPackage.path);
            }
        }
        if (!ownsReference) { return true; }
        if (frame.leaves.empty()) {
            return validation::AssignError(error, "decode reference frame contains no reusable leaf data");
        }
        const auto published = m_referenceCache->Publish(*key, std::move(frame));
        if (published == nullptr) {
            return validation::AssignError(error, "failed to publish decode reference frame");
        }
        return true;
    }

    void PruneCompletedFrameStates() {
        if (!m_activeFramePackage.has_value()) {
            return;
        }
        const auto currentFrameIndex = m_activeFramePackage->frameIndex;
        const auto attributeKeyFrameIndex = m_activeFramePackage->attributeKeyFrameIndex;
        const auto geometryKeyFrameIndex = m_activeFramePackage->geometryKeyFrameIndex;
        std::unordered_set<std::string> retainedTopologyKeys;
        for (const auto& [key, stateOwner] : m_leafStates) {
            (void)key;
            const auto& state = *stateOwner;
            const bool retainBaseState = state.frameIndex == currentFrameIndex ||
                (m_activeFramePackage->attributeTemporalRole != TemporalFieldRole::SingleFrame &&
                 state.frameIndex == attributeKeyFrameIndex) ||
                (m_activeFramePackage->geometryTemporalRole != TemporalFieldRole::SingleFrame &&
                 state.frameIndex == geometryKeyFrameIndex);
            if (retainBaseState && !state.topologyReferenceKey.empty()) {
                retainedTopologyKeys.insert(state.topologyReferenceKey);
            }
        }
        for (auto iterator = m_leafStates.begin(); iterator != m_leafStates.end();) {
            const auto& state = *iterator->second;
            const bool retainBaseState = state.frameIndex == currentFrameIndex ||
                (m_activeFramePackage->attributeTemporalRole != TemporalFieldRole::SingleFrame &&
                 state.frameIndex == attributeKeyFrameIndex) ||
                (m_activeFramePackage->geometryTemporalRole != TemporalFieldRole::SingleFrame &&
                 state.frameIndex == geometryKeyFrameIndex);
            const bool retain = retainBaseState ||
                (!state.workspace->topologyBorrowed &&
                 !state.topologyReferenceKey.empty() &&
                 retainedTopologyKeys.contains(state.topologyReferenceKey));
            if (!retain) {
                iterator = m_leafStates.erase(iterator);
                continue;
            }
            ++iterator;
        }
        if (m_state.decodeTopologyReferenceStore != nullptr) {
            m_state.decodeTopologyReferenceStore->RetainKeys(retainedTopologyKeys);
        }
    }

    void ResetFrameAssembly() noexcept {
        m_frameAssembly = nullptr;
        m_frameBegun = false;
    }

    DataCodecReferenceState m_state;
    std::shared_ptr<DecodeReferenceCache> m_referenceCache;
    struct ActiveFrameMetadata {
        std::uint32_t frameIndex{0u};
        TemporalFieldRole geometryTemporalRole{TemporalFieldRole::SingleFrame};
        std::uint32_t geometryKeyFrameIndex{0u};
        TemporalFieldRole attributeTemporalRole{TemporalFieldRole::SingleFrame};
        std::uint32_t attributeKeyFrameIndex{0u};
    };
    std::shared_ptr<const FrameIdentityMap> m_frameIdentities;
    std::optional<ActiveFrameMetadata> m_activeFramePackage;
    std::optional<std::uint32_t> m_outputFrameIndex;
    std::unordered_map<std::string, std::unique_ptr<LeafDecodeState>> m_leafStates;
    IFramePackageDecodeAssembly* m_frameAssembly{nullptr};
    bool m_frameBegun{false};
};

} // namespace datacodec

#endif
