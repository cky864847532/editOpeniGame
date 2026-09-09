#ifndef DATACODEC_RUNTIME_WORKSPACE_DECODELEAFWORKSPACE_H
#define DATACODEC_RUNTIME_WORKSPACE_DECODELEAFWORKSPACE_H

#include "DataCodec/Common/Views/TopologyViews.h"
#include "DataCodec/Storage/ByteStore/ByteStore.h"
#include "DataCodec/Runtime/Cache/DecodeCache/DecodedAttributeCacheSet.h"
#include "DataCodec/Runtime/Cache/DecodeCache/DecodedGeometryCache.h"
#include "DataCodec/Runtime/Cache/DecodeCache/DecodedTopologyCache.h"
#include "DataCodec/Runtime/Cache/CacheResources.h"
#include "DataCodec/Runtime/Failure/FailureCleanable.h"
#include "DataCodec/Runtime/Execution/DataCodecExecutionResources.h"
#include "DataCodec/Codec/NumericArray/NumericArraySource.h"
#include "DataCodec/Storage/LeafPackage/LeafPackage.h"
#include "DataCodec/Storage/ByteIO/Window/WindowRuntimeParams.h"
#include "DataCodec/API/Params/CodecControlParams.h"
#include "DataCodec/API/Params/CodecStorageParams.h"
#include "DataCodec/Validation/Policy/CodecValidationPolicy.h"
#include "DataCodec/Runtime/Workspace/LeafPackageFields.h"

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>
namespace datacodec {

struct DecodeLeafWorkspace : IFailureCleanable {
    LeafPackageFields packageFields;
    DecodedGeometryCache geometry;
    DecodedAttributeCacheSet attributes;
    std::shared_ptr<DecodedTopologyCache> topology;
    bool topologyBorrowed{false};

    [[nodiscard]] const CodecStorageParams& StorageParams() const noexcept { return m_storageParams; }
    void SetStorageParams(CodecStorageParams storageParams) noexcept {
        m_storageParams = std::move(storageParams);
        m_committedAttributes.assign(m_storageParams.attrParams.size(), 0u);
    }
    [[nodiscard]] const CacheResources& CacheResourcesRef() const noexcept { return m_cacheResources; }
    [[nodiscard]] CacheResources& CacheResourcesRef() noexcept { return m_cacheResources; }
    [[nodiscard]] const CodecValidationPolicy& ValidationPolicy() const noexcept { return m_validationPolicy; }
    void SetValidationPolicy(CodecValidationPolicy policy) noexcept { m_validationPolicy = policy; }
    [[nodiscard]] bytestore::ByteStoreSession& ByteStoreSessionRef() noexcept { return m_byteStoreSession; }
    [[nodiscard]] const bytestore::ByteStoreSession& ByteStoreSessionRef() const noexcept { return m_byteStoreSession; }
    [[nodiscard]] ScratchByteBufferPool& ScratchBytePool() const { return m_cacheResources.ScratchBytePool(); }
    [[nodiscard]] std::stop_token StopToken() const noexcept { return m_run ? m_run->StopToken() : std::stop_token{}; }
    [[nodiscard]] bool StopRequested() const noexcept {
        return m_run && m_run->Stopped();
    }
    void RequestStop() noexcept { if (m_run && !m_run->Stopped()) { m_run->RequestStop(); } }
    [[nodiscard]] bool MatchesLeafPackage(const LeafPackage* leafPackage) const noexcept {
        return m_leafPackage == leafPackage && leafPackage != nullptr;
    }

    void SetPreparedAttributePayload(
        const LeafPackageField* field, std::shared_ptr<bytestore::IByteSource> owner) noexcept {
        m_attributePayloadField = field;
        m_attributePayloadOwner = std::move(owner);
    }

    void ClearPreparedAttributePayload() noexcept {
        m_attributePayloadField = nullptr;
        m_attributePayloadOwner.reset();
    }

    void PrepareSupplementRun(
        const CodecValidationPolicy validationPolicy) {
        m_failureCleanupCompleted.store(false, std::memory_order_release);
        m_validationPolicy = validationPolicy;
    }

    [[nodiscard]] bool HasField(const FieldType type, const std::size_t ordinal = 0u) const {
        return packageFields.HasField(type, ordinal);
    }

    [[nodiscard]] bool AttributeCommitted(const std::size_t attrIndex) const noexcept {
        return attrIndex < m_committedAttributes.size() && m_committedAttributes[attrIndex] != 0u;
    }

    void MarkAttributesCommitted(const std::span<const std::size_t> attrIndices) noexcept {
        for (const auto attrIndex : attrIndices) {
            if (attrIndex < m_committedAttributes.size()) {
                m_committedAttributes[attrIndex] = 1u;
            }
        }
    }

    [[nodiscard]] DecodedTopologyCache& MutableTopology() {
        topologyBorrowed = false;
        if (topology == nullptr) {
            topology = std::make_shared<DecodedTopologyCache>();
        }
        return *topology;
    }

    void BindTopologyReference(std::shared_ptr<DecodedTopologyCache> reference) {
        topology = std::move(reference);
        topologyBorrowed = true;
    }

    [[nodiscard]] const DecodedTopologyCache* TopologyForCommit() const noexcept {
        return topology != nullptr && topology->complete ? topology.get() : nullptr;
    }

    void Reset(const LeafPackage* leafPackage = nullptr) {
        m_failureCleanupCompleted.store(false, std::memory_order_release);
        m_attributePayloadOwner.reset();
        m_attributePayloadField = nullptr;
        m_storageParams = {};
        m_committedAttributes.clear();
        m_validationPolicy = {};
        m_byteStoreSession.Reset();
        packageFields.Reset(leafPackage);
        geometry.Release();
        attributes.Reset();
        if (topology != nullptr && !topologyBorrowed) {
            topology->Release();
        }
        topology.reset();
        topologyBorrowed = false;
        m_leafPackage = leafPackage;
    }

    void CleanupOnFailure() noexcept override {
        if (m_failureCleanupCompleted.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        RequestStop();
        m_storageParams = {};
        m_committedAttributes.clear();
        m_validationPolicy = {};
        m_byteStoreSession.ReleaseAll();
        m_attributePayloadOwner.reset();
        m_attributePayloadField = nullptr;
        packageFields.Clear();
        geometry.Release();
        attributes.Reset();
        if (topology != nullptr && !topologyBorrowed) {
            topology->Release();
        }
        topology.reset();
        topologyBorrowed = false;
        m_leafPackage = nullptr;
    }

private:
    friend class RunBinding<DecodeLeafWorkspace>;
    void BindRun(DataCodecExecutionResources& run) {
        if (m_run) {
            run.RecordFailure(MakeCodecFailureRecord(CodecErrorCode::PipelineFailure,
                "workspace-already-bound", "DecodeLeafWorkspace", "workspace already belongs to an active run"), true);
            throw std::logic_error("decode workspace already bound");
        }
        m_byteStoreSession.BindRun(run);
        m_cacheResources.BindRun(run);
        m_run = &run;
    }
    void UnbindRun() noexcept {
        m_byteStoreSession.UnbindRun();
        m_cacheResources.UnbindRun();
        m_run = nullptr;
    }

    CodecStorageParams m_storageParams;
    std::vector<std::uint8_t> m_committedAttributes;
    CodecValidationPolicy m_validationPolicy;
    CacheResources m_cacheResources;
    bytestore::ByteStoreSession m_byteStoreSession;
    const LeafPackage* m_leafPackage{nullptr};
    const LeafPackageField* m_attributePayloadField{nullptr};
    std::shared_ptr<bytestore::IByteSource> m_attributePayloadOwner;
    DataCodecExecutionResources* m_run{nullptr};
    std::atomic_bool m_failureCleanupCompleted{false};
};

} // namespace datacodec

#endif
