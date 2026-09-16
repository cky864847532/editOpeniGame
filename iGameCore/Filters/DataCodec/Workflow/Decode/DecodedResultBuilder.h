#ifndef DATACODEC_WORKFLOW_DECODE_DECODEDRESULTBUILDER_H
#define DATACODEC_WORKFLOW_DECODE_DECODEDRESULTBUILDER_H

#include "DataCodec/Workflow/Decode/IFramePackageDecodeAssembly.h"
#include "DataCodec/API/Output/DecodedData.h"
#include "DataCodec/Runtime/Cache/DecodeCache/DecodedGeometryCache.h"
#include "DataCodec/Runtime/Cache/DecodeCache/DecodedTopologyCache.h"
#include "DataCodec/Storage/ByteStore/DecodedBufferAccess.h"

namespace datacodec {

// 构造器只在核心执行期间存在，提交时交付普通值类型
class DecodedLeafBuilder final : public IDecodeAdapter {
public:
    explicit DecodedLeafBuilder(std::shared_ptr<const ICellTypeMapping> mapping = {})
        : m_mapping(std::move(mapping)) {}

    DecodedLeaf output;

    bool SetMeshType(MeshType type, std::string*) override { output.meshType = type; return true; }
    void ResetOutput() override { output = {}; }
    bool Commit(std::string*) override { return true; }
    bool SupportsPolyhedronTopology() const override { return true; }

    bool AcceptGeometry(DecodedGeometryCache& geometry, bytestore::ByteStoreSession& stores,
                        std::string* error) {
        output.geometry.dataType = geometry.dataType;
        output.geometry.pointCount = geometry.pointCount;
        output.geometry.dimension = geometry.dimension;
        return DecodedBufferAccess::Export(*geometry.bytes, stores, output.geometry.values, error);
    }

    bool AcceptTopology(const DecodedTopologyCache& source, bytestore::ByteStoreSession& stores,
                        std::string* error) {
        auto& target = output.topology;
        target.kind = static_cast<DecodedTopology::Kind>(source.kind);
        target.cellCount = source.cellCount;
        target.structuredAxisSize = source.structuredAxisSize;
        const auto transfer = [&](const auto& store, DecodedBuffer& buffer) {
            return !store || DecodedBufferAccess::Export(*store, stores, buffer, error);
        };
        if (source.kind == DecodedTopologyCache::Kind::Connectivity) {
            return transfer(source.connectivity, target.connectivity) &&
                transfer(source.offsets, target.offsets) && transfer(source.cellTypes, target.cellTypes) &&
                transfer(source.cellPolynomialOrders, target.polynomialOrders);
        }
        if (source.kind == DecodedTopologyCache::Kind::Polyhedron) {
            const auto& poly = source.polyhedron;
            target.cellCount = static_cast<std::size_t>(poly.cellCount);
            return transfer(poly.uniqueVertexCounts.ByteSource(), target.uniqueVertexCounts) &&
                transfer(poly.cellFaceCounts.ByteSource(), target.cellFaceCounts) &&
                transfer(poly.faceVertexCounts.ByteSource(), target.faceVertexCounts) &&
                transfer(poly.cellUniqueVertexIds.ByteSource(), target.cellUniqueVertexIds) &&
                transfer(poly.localFaceVertexIds.ByteSource(), target.localFaceVertexIds);
        }
        return true;
    }

    bool AcceptAttribute(std::size_t index, const AttrStorageParams& meta, bytestore::IByteSource& source,
                         bytestore::ByteStoreSession& stores, std::string* error) {
        DecodedAttribute attribute{.sourceIndex = index, .metadata = meta};
        if (!DecodedBufferAccess::Export(source, stores, attribute.values, error)) { return false; }
        for (auto& existing : output.attributes) {
            if (existing.sourceIndex == index) { existing = std::move(attribute); return true; }
        }
        output.attributes.push_back(std::move(attribute));
        return true;
    }

    bool ResolveCellType(CellTypeRaw type, CellTypeCodecEntry& entry) const override {
        return m_mapping && m_mapping->ResolveCellType(type, entry);
    }
    bool ResolveCellSizeFromPolynomialOrder(CellTypeRaw type, std::uint16_t order, int& size) const override {
        return m_mapping && m_mapping->ResolveCellSizeFromPolynomialOrder(type, order, size);
    }
    bool EncodeCellTypeFamilyLocal(CellTypeRaw type, CellTypeFamilyCode& family, CellTypeLocalCode& local) const override {
        return m_mapping && m_mapping->EncodeCellTypeFamilyLocal(type, family, local);
    }
    bool DecodeCellTypeFamilyLocal(CellTypeFamilyCode family, CellTypeLocalCode local, CellTypeRaw& type) const override {
        return m_mapping && m_mapping->DecodeCellTypeFamilyLocal(family, local, type);
    }
    bool EncodeCellPolynomialOrderLocal(CellTypeRaw type, std::uint16_t order, CellTypeLocalCode& local) const override {
        return m_mapping && m_mapping->EncodeCellPolynomialOrderLocal(type, order, local);
    }
    bool DecodeCellPolynomialOrderLocal(CellTypeRaw type, CellTypeLocalCode local, std::uint16_t& order) const override {
        return m_mapping && m_mapping->DecodeCellPolynomialOrderLocal(type, local, order);
    }

    // 核心结果必须直接接管缓存，范围回放代表调用链接入错误
    bool BeginPoints(std::size_t, std::size_t, DataType, std::string* e) override { return Unexpected(e); }
    bool WritePointsRange(std::size_t, std::size_t, const void*, std::string* e) override { return Unexpected(e); }
    bool EndPoints(std::string* e) override { return Unexpected(e); }
    bool BeginTopology(std::size_t, std::size_t, bool, std::string* e) override { return Unexpected(e); }
    bool WriteConnectivityRange(std::size_t, const IndexType*, std::size_t, std::string* e) override { return Unexpected(e); }
    bool WriteOffsetsRange(std::size_t, const IndexType*, std::size_t, std::string* e) override { return Unexpected(e); }
    bool WriteCellTypesRange(std::size_t, const IndexType*, std::size_t, std::string* e) override { return Unexpected(e); }
    bool WriteCellPolynomialOrdersRange(std::size_t, const std::uint16_t*, std::size_t, std::string* e) override { return Unexpected(e); }
    bool EndTopology(std::string* e) override { return Unexpected(e); }
    bool SetStructuredAxisSize(const int[3], std::string* e) override { return Unexpected(e); }
    bool BeginPolyhedronTopology(std::size_t, std::string* e) override { return Unexpected(e); }
    bool WritePolyhedronCellBatch(std::size_t, const PolyhedronTopologyView&, std::string* e) override { return Unexpected(e); }
    bool EndPolyhedronTopology(std::string* e) override { return Unexpected(e); }
    bool BeginAttribute(std::size_t, const AttrStorageParams&, std::string* e) override { return Unexpected(e); }
    bool WriteAttributeRange(std::size_t, std::size_t, std::size_t, const void*, std::size_t, std::string* e) override { return Unexpected(e); }
    bool EndAttribute(std::size_t, std::string* e) override { return Unexpected(e); }

private:
    static bool Unexpected(std::string* error) {
        return validation::AssignError(error, "owned decode result must receive completed storage");
    }
    std::shared_ptr<const ICellTypeMapping> m_mapping;
};

class DecodedDataBuilder final : public IFramePackageDecodeAssembly {
public:
    explicit DecodedDataBuilder(std::shared_ptr<const ICellTypeMapping> mapping = {})
        : m_mapping(std::move(mapping)) {}
    DecodedData output;

    bool BeginFramePackage(const FramePackage& package, std::string*) override {
        output = {};
        output.frameIndex = package.frameIndex;
        output.timeValue = package.timeValue;
        output.rootName = package.rootName;
        return true;
    }
    bool AddBranch(const FramePackageBranchRecord& branch, std::string*) override {
        output.branches.push_back({branch.path, branch.name});
        return true;
    }
    std::unique_ptr<IDecodeAdapter> CreateLeafAdapter(const FramePackageLeafRecord& leaf,
        const LeafPackage&, std::string*) override {
        auto result = std::make_unique<DecodedLeafBuilder>(m_mapping);
        result->output.path = leaf.path;
        result->output.name = leaf.name;
        result->output.topologyOwnerFrameIndex = leaf.ownerFrameIndex;
        result->output.topologyMode = leaf.topologyMode;
        return result;
    }
    bool CommitLeaf(const FramePackageLeafRecord&, IDecodeAdapter& adapter, std::string*) override {
        output.leaves.push_back(std::move(static_cast<DecodedLeafBuilder&>(adapter).output));
        return true;
    }
    bool EndFramePackage(std::string*) override { return true; }
    void AbortFramePackage() override { output = {}; }
private:
    std::shared_ptr<const ICellTypeMapping> m_mapping;
};

} // 命名空间 datacodec
#endif
