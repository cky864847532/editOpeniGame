#include "DataCodec/API/Entry/DataCodecEncodeEntry.h"
#include "DataCodec/API/Entry/EncodeStorageAnalysis.h"
#include "DataCodec/Workflow/Encode/EncodeStoragePlan.h"
#include "DataCodec/Workflow/Leaf/LeafEncodeExecutor.h"
#include "DataCodec/Codec/Remap/Common/MortonRemapBuilder.h"
#include "DataCodec/Codec/Reference/AttributeReferenceScheduleBuilder.h"
#include "DataCodec/Codec/Topology/Connectivity/ConnectivityTopologyBlockEncode.h"
#include <iostream>
#include <stdexcept>

namespace datacodec::test {
namespace {

void Ensure(bool condition, const char* text) {
    if (!condition) { throw std::runtime_error(text); }
}

// 数值读取立即失败，确保分析只使用元数据
class MetadataAttribute final : public IEncodeAttrView {
public:
    std::string name{"value"};
    std::size_t count{16u};
    int dimension{1};
    DataType type{DataType::Float32};
    AttrAttachment attachment{AttrAttachment::Point};
    std::string GetName() const override { return name; }
    DataType GetDataType() const override { return type; }
    AttrRole GetRole() const override { return AttrRole::Scalar; }
    AttrAttachment GetAttachType() const override { return attachment; }
    int GetComponentCount() const override { return dimension; }
    std::size_t GetElementCount() const override { return count; }
    void GetTuple(std::size_t, double*) const override { throw std::runtime_error("unexpected attribute read"); }
};

class MetadataAdapter final : public IEncodeAdapter {
public:
    std::size_t points{16u}, cells{0u}, faces{0u};
    bool polyhedron{false}, structured{false};
    std::vector<MetadataAttribute> pointFields, cellFields;
    std::string GetName() const override { return "metadata_only"; }
    MeshType GetMeshType() const override {
        return structured ? MeshType::StructuredMesh : (polyhedron ? MeshType::PolyhedronMesh : MeshType::UnstructuredMesh);
    }
    bool IsStructuredMesh() const override { return structured; }
    bool IsPolyhedronMesh() const override { return polyhedron; }
    std::size_t GetNumberOfPoints() const override { return points; }
    std::size_t GetNumberOfCells() const override { return cells; }
    std::size_t GetNumberOfFaces() const override { return faces; }
    std::size_t GetCellIdBufferSize() const override { return 0u; }
    std::size_t GetCellFaceBufferSize() const override { return 0u; }
    const IndexType* GetCellIdBufferPtr() const override { throw std::runtime_error("unexpected connectivity read"); }
    const IndexType* GetCellIdOffsetPtr() const override { throw std::runtime_error("unexpected offset read"); }
    bool IsFixedCellSize() const override { return false; }
    int GetFixedCellSize() const override { return 0; }
    void GetPoint(std::size_t, double*) const override { throw std::runtime_error("unexpected geometry read"); }
    bool DescribeTopology(TopologyInputDescriptor& output, std::string*) const override {
        output = {}; output.pointCount = points; output.cellCount = cells; output.polyhedron = polyhedron;
        return true;
    }
    std::size_t GetNumberOfPointAttrs() const override { return pointFields.size(); }
    std::size_t GetNumberOfCellAttrs() const override { return cellFields.size(); }
    const IEncodeAttrView& GetPointAttr(std::size_t i) const override { return pointFields.at(i); }
    const IEncodeAttrView& GetCellAttr(std::size_t i) const override { return cellFields.at(i); }
    void ResetInput() override {}
};

class MetadataTree final : public IBlockTreeAdapter {
public:
    MetadataAdapter data;
    void EnumerateLeafPaths(const std::function<void(const BlockPath&)>& visitor) const override {
        visitor("first"); visitor("second");
    }
    std::unique_ptr<IEncodeAdapter> GetLeaf(const BlockPath&) const override { return std::make_unique<MetadataAdapter>(data); }
};

class CountingOutput final : public IByteRangeOutput {
public:
    std::size_t writes{0u};
    bool WriteAt(std::uint64_t, std::span<const std::uint8_t>, std::string*) override { ++writes; return true; }
    bool Finalize(std::uint64_t, std::string*) override { ++writes; return true; }
};

std::uint64_t Bound(const EncodeStorageAnalysisResult& result) {
    if (!result.success || !result.provenLowerBoundBytes) {
        throw std::runtime_error(result.failure ? FormatCodecFailure(*result.failure) : "missing encode bound");
    }
    return *result.provenLowerBoundBytes;
}

}

int RunDataCodecEncodeStorageAnalysisTests() {
    try {
        MetadataAdapter adapter;
        adapter.pointFields.resize(2u);
        adapter.pointFields[1].name = "second";
        EncodeRequest request;
        request.input = EncodeInput::LeafAdapter(&adapter);
        request.configuration.pipelineControl.pointOrder = EncodePointOrderMode::Original;
        const auto samples = AnalyzeEncodeStorage(request);
        const auto expected = 16u * sizeof(std::size_t) + 2u * 16u * sizeof(float);
        Ensure(Bound(samples) == expected && samples.peakAllocations.size() == 3u, "sample group exact coexistence");
        Ensure(!samples.unresolved.empty() && samples.inspectedLeafCount == 1u, "partial scope is explicit");
        Ensure(CheckEncodeStorageLowerBound(samples, expected - 1u).has_value(), "below bound rejected");
        Ensure(!CheckEncodeStorageLowerBound(samples, expected) &&
            !CheckEncodeStorageLowerBound(samples, expected + 1u) &&
            !CheckEncodeStorageLowerBound(samples, std::nullopt), "equal, higher and unrestricted allowed by preflight");

        CountingOutput output;
        request.output = EncodeOutput::ByteRange(output);
        request.resources = {CodecResourceMode::Fixed, 1u, expected - 1u};
        auto rejected = Encode(request);
        Ensure(!rejected.success && rejected.failure && output.writes == 0u &&
            std::string_view(rejected.failure->reason.data()) == "encode.storage-limit-too-small" &&
            rejected.failure->requestedBytes == expected, "core rejects before reading data or writing output");

        MetadataTree tree;
        tree.data = adapter;
        request.input = EncodeInput::BlockTreeAdapter(&tree);
        const auto treeAnalysis = AnalyzeEncodeStorage(request);
        Ensure(Bound(treeAnalysis) == expected && treeAnalysis.inspectedLeafCount == 2u, "independent leaves use max");
        request.input = EncodeInput::LeafAdapter(&adapter);
        request.attributeSelection = AttributeSelectionMode::Explicit;
        request.attributeTargets = {{.attrIndex = 0u}};
        Ensure(Bound(AnalyzeEncodeStorage(request)) == 0u, "one selected field creates no sample group");
        request.attributeTargets.push_back({.attrIndex = 0u});
        Ensure(!AnalyzeEncodeStorage(request).success, "duplicate selection is invalid");
        request.attributeTargets.clear();
        request.attributeSelection = AttributeSelectionMode::AllAvailable;

        adapter.cellFields = adapter.pointFields;
        for (auto& field : adapter.cellFields) { field.attachment = AttrAttachment::Cell; }
        Ensure(Bound(AnalyzeEncodeStorage(request)) == expected, "point and cell sample groups release separately");
        adapter.pointFields[1].type = DataType::Float64;
        adapter.cellFields.clear();
        Ensure(Bound(AnalyzeEncodeStorage(request)) == 0u, "different scalar types do not share a sample group");
        adapter.pointFields[1].type = DataType::Float32;

        request.configuration.controlParams.geomControl.regionControl.regions.resize(1u);
        request.configuration.controlParams.geomControl.regionRuns.resize(20u);
        const auto regionBytes = 20u * sizeof(RegionRun);
        Ensure(Bound(AnalyzeEncodeStorage(request)) == std::max<std::size_t>(expected, regionBytes),
            "region and reference phases use max, not sum");
        request.configuration.controlParams.geomControl.regionControl.regions.clear();
        Ensure(Bound(AnalyzeEncodeStorage(request)) == expected, "unused region list is not allocated");

        adapter.pointFields.clear();
        adapter.points = 100u;
        adapter.cells = 40u;
        request.configuration.pipelineControl.pointOrder = EncodePointOrderMode::Morton;
        request.configuration.pipelineControl.cellOrder = EncodeCellOrderMode::Morton;
        const auto remap = AnalyzeEncodeStorage(request);
        Ensure(Bound(remap) == 140u * sizeof(IndexType) && remap.peakAllocations.size() == 2u,
            "topology inverse and cell order coexist");
        adapter.polyhedron = true;
        adapter.faces = 500u;
        Ensure(Bound(AnalyzeEncodeStorage(request)) == 500u, "polyhedron only requires visited mask from metadata");
        const auto reused = encodestorage::AnalyzeLeaf(adapter, request.configuration.controlParams,
            request.configuration.pipelineControl, {}, 0u, {}, false);
        Ensure(Bound(reused) == 0u, "reused topology excludes visited and topology remap arrays");
        adapter.polyhedron = false;
        adapter.structured = true;
        Ensure(Bound(AnalyzeEncodeStorage(request)) == 0u, "structured pipeline forces original order");
        adapter.structured = false;
        adapter.cells = 0u;
        adapter.points = mortonremap::ResolveLeafBudgetElements() + 1u;
        Ensure(Bound(AnalyzeEncodeStorage(request)) == mortonremap::MortonHighSliceRecords() * mortonremap::kMortonRunRecordBytes,
            "unknown histogram has a strict slab floor without charging the full histogram");
        request.configuration.pipelineControl.pointOrder = EncodePointOrderMode::Original;
        Ensure(Bound(AnalyzeEncodeStorage(request)) == 0u, "borrowed geometry and file-capable stores impose no whole-array bound");

        adapter.points = 100000u;
        adapter.pointFields.resize(2u);
        for (auto& field : adapter.pointFields) { field.count = adapter.points; }
        request.configuration.controlParams.attrReference.intraField.sampleCount = 100000u;
        auto partial = AnalyzeEncodeStorage(request);
        Ensure(Bound(partial) == 100000u * sizeof(std::size_t) &&
            std::find(partial.unresolved.begin(), partial.unresolved.end(), EncodeStorageUnknown::ReferenceSampleEnumeration) != partial.unresolved.end(),
            "large sample requests retain an exact index bound without a large preflight allocation");
        const std::array<AttributeTarget, 2> temporalTargets{{{.attrIndex = 0u}, {.attrIndex = 1u}}};
        const auto temporal = encodestorage::AnalyzeLeaf(adapter, request.configuration.controlParams,
            request.configuration.pipelineControl, temporalTargets, 0u, {}, true, TemporalFieldRole::KeyFrame);
        Ensure(Bound(temporal) == 0u, "temporal roles skip intra-field sampling");

        std::stop_source cancel;
        cancel.request_stop();
        const auto cancelled = AnalyzeEncodeStorage(request, cancel.get_token());
        Ensure(!cancelled.success && cancelled.cancelled && !cancelled.provenLowerBoundBytes &&
            cancelled.failure && cancelled.failure->cancelled, "cancelled analysis gives no usable number");
        std::size_t overflowBytes = 0u;
        Ensure(!topocodec::CalculateTopologyRemapStorageBytes(std::numeric_limits<std::size_t>::max(), overflowBytes),
            "shared topology capacity arithmetic detects overflow");
        Ensure(!CalculateNumericRegionStorageBytes(std::numeric_limits<std::size_t>::max(), overflowBytes),
            "shared region capacity arithmetic detects overflow");

        // 实际采样分配的额度与预检一致，降一字节时最后一个共存 owner 无法准入
        for (const bool enough : {true, false}) {
            DataCodecExecutionResources root(ResolvedResourceConfiguration{
                {enough ? expected : expected - 1u, 1u, 1u}, enough ? expected : expected - 1u, 1u, false});
            CodecRunScope run(root);
            bytestore::ByteStoreSession stores;
            stores.BindRun(root);
            std::shared_ptr<bytestore::MemoryStore> indicesOwner;
            std::span<std::size_t> indices;
            std::string error;
            Ensure(CreateAttributeReferenceArray(stores, 16u, "indices", indicesOwner, indices, &error), "index allocation");
            AttrStorageParams meta;
            meta.dataType = DataType::Float32;
            meta.dimension = 1;
            meta.elementCount = 16u;
            AttributeReferenceFieldSample first, second;
            Ensure(PrepareAttributeReferenceFieldSample(meta, 16u, stores, first, &error), "first field allocation");
            Ensure(PrepareAttributeReferenceFieldSample(meta, 16u, stores, second, &error) == enough,
                "actual sample allocation follows exact bound");
        }
        for (const auto mode : {CodecResourceMode::Adaptive, CodecResourceMode::Unlimited}) {
            CodecResourceParams params;
            params.mode = mode;
            params.maxComputeThreads = 1u;
            DataCodecExecutionResources root(params);
            Ensure(!root.FixedStorageLimitBytes(), "adaptive and unlimited have no fixed preflight limit");
        }
        std::cout << "encode storage lower-bound tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
}
