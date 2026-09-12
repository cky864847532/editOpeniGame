#include "DataCodec/Workflow/Encode/EncodeStoragePlan.h"
#include "DataCodec/API/Params/CodecParamFactories.h"
#include "DataCodec/Workflow/Encode/EncodePipelineBinding.h"
#include "DataCodec/Codec/Reference/AttributeReferenceScheduleBuilder.h"
#include "DataCodec/Codec/Remap/Common/MortonRemapBuilder.h"
#include "DataCodec/Codec/Topology/Connectivity/ConnectivityTopologyBlockEncode.h"

namespace datacodec::encodestorage {
namespace {

void Unknown(EncodeStorageAnalysisResult& result, const EncodeStorageUnknown item) {
    if (std::find(result.unresolved.begin(), result.unresolved.end(), item) == result.unresolved.end()) {
        result.unresolved.push_back(item);
    }
}

bool Observe(EncodeStorageAnalysisResult& result, const std::string& stage,
    std::vector<EncodeStorageAllocation> allocations, std::string& error) {
    std::size_t sum = 0u;
    for (const auto& allocation : allocations) {
        if (!validation::CheckedAddSizeT(sum, allocation.bytes, sum, "encode storage lower bound", &error)) {
            return false;
        }
    }
    if (sum > result.provenLowerBoundBytes.value_or(0u)) {
        result.provenLowerBoundBytes = sum;
        result.peakStage = stage;
        result.peakAllocations = std::move(allocations);
    }
    return true;
}

bool Regions(EncodeStorageAnalysisResult& result, const NumericArrayControlParams& control,
    const std::string& name, std::string& error) {
    if (control.regionControl.regions.empty()) { return true; }
    std::size_t bytes = 0u;
    return CalculateNumericRegionStorageBytes(control.regionRuns.size(), bytes, &error) &&
        Observe(result, "numeric_regions:" + name, {{"numeric_region_runs", bytes}}, error);
}

bool Samples(EncodeStorageAnalysisResult& result, const std::vector<AttrStorageParams>& metas,
    const AttrReferenceControlParams& dependency, std::stop_token stop, std::string& error) {
    if (!IsIntraFieldReferenceEnabled(dependency)) { return true; }
    std::vector<AttributeReferenceSampleGroup> groups;
    for (std::size_t index = 0u; index < metas.size(); ++index) {
        if (stop.stop_requested()) { return false; }
        const auto& meta = metas[index];
        if (!IsAttributeReferenceSampleFieldEligible(meta, dependency.intraField.codec)) { continue; }
        auto group = std::find_if(groups.begin(), groups.end(), [&](const auto& item) {
            const auto& representative = metas[item.representativeFieldIndex];
            return representative.attachmentType == meta.attachmentType &&
                HasMatchingAttributeReferenceSampleLayout(representative, meta);
        });
        if (group == groups.end()) { groups.push_back({index, {index}}); }
        else { group->fieldIndices.push_back(index); }
    }
    // 分析最多枚举固定数量的采样索引，超出时只保留必需索引存储的精确下界
    constexpr std::size_t enumerationLimit = 65536u;
    for (const auto& group : groups) {
        if (stop.stop_requested()) { return false; }
        if (group.fieldIndices.size() < 2u) { continue; }
        const auto& meta = metas[group.representativeFieldIndex];
        std::size_t elements = 0u, indexBytes = 0u;
        if (!TryParamSizeToSizeT(meta.elementCount, elements)) {
            return validation::AssignError(&error, "attribute sample domain exceeds this platform size limit");
        }
        const auto count = std::min(elements, std::max<std::size_t>(1u, dependency.intraField.sampleCount));
        if (!validation::CheckedMulSizeT(count, sizeof(std::size_t), indexBytes,
                "attribute sample index bytes", &error)) { return false; }
        std::vector<EncodeStorageAllocation> allocations{{"attribute_sample_indices", indexBytes}};
        if (count <= enumerationLimit) {
            std::vector<std::size_t> indices(count);
            std::span<std::size_t> uniqueIndices(indices);
            if (!BuildAttributeReferenceSampleIndices(meta.elementCount, dependency.intraField.sampleCount,
                    uniqueIndices, &error)) { return false; }
            for (const auto index : group.fieldIndices) {
                std::size_t bytes = 0u;
                if (!CalculateAttributeReferenceFieldSampleBytes(metas[index], uniqueIndices.size(), bytes, &error)) {
                    return false;
                }
                allocations.push_back({"attribute_reference_sample:" + metas[index].name, bytes});
            }
        } else {
            Unknown(result, EncodeStorageUnknown::ReferenceSampleEnumeration);
        }
        if (!Observe(result, "attribute_reference_samples:" + meta.name, std::move(allocations), error)) { return false; }
    }
    return true;
}

}

EncodeStorageAnalysisResult AnalyzeLeaf(
    const IEncodeAdapter& adapter, const CodecControlParams& controls,
    const EncodePipelineControlParams& pipeline, const std::span<const AttributeTarget> targets,
    const std::uint32_t frameIndex, const BlockPath& path, const bool includeTopology,
    const TemporalFieldRole attributeRole, const std::stop_token stop) {
    EncodeStorageAnalysisResult result;
    result.peakFrameIndex = frameIndex;
    result.peakLeafPath = path;
    const auto fail = [&](const std::string& message) {
        result.success = false;
        result.cancelled = stop.stop_requested();
        result.provenLowerBoundBytes.reset();
        result.failure = MakeCodecFailureRecord(CodecErrorCode::InvalidInput,
            result.cancelled ? "encode.storage-analysis.cancelled" : "encode.storage-analysis",
            "EncodeStorageAnalysis", result.cancelled ? "encode storage analysis cancelled" : message, result.cancelled);
        return result;
    };
    try {
        if (stop.stop_requested()) { return fail({}); }
        const auto descriptor = ResolveEncodePipelineDescriptor(adapter, controls, pipeline,
            EncodePipelineOutputKind::LeafPackage, includeTopology);
        std::string error;
        if (!ValidateEncodePipelineDescriptor(descriptor, &error) || !ValidateEncodeAlgorithmParams(controls, &error)) {
            return fail(error);
        }
        std::vector<std::size_t> selected;
        for (const auto& target : targets) {
            if (MatchesAttributeTargetLeaf(target, frameIndex, path)) { selected.push_back(target.attrIndex); }
        }
        CodecStorageParams metadata;
        if (!CodecStorageParamsFactory::TryFromEncodeAdapter(adapter, selected, metadata, &error)) { return fail(error); }
        result.provenLowerBoundBytes = 0u;
        result.unresolved = {EncodeStorageUnknown::EncodedPayloadAndOutput,
            EncodeStorageUnknown::StorageBackendAndGrowth};
        const auto points = adapter.GetNumberOfPoints();
        const auto cells = adapter.GetNumberOfCells();
        if (descriptor.pointOrder == EncodePointOrderMode::Morton) {
            if (!Observe(result, "point_morton_high_slab",
                    {{"morton_high_slab", mortonremap::MortonHighSlabLowerBound(points)}}, error)) { return fail(error); }
            Unknown(result, EncodeStorageUnknown::MortonBucketDistribution);
        }
        if (descriptor.cellOrder == EncodeCellOrderMode::Morton) {
            if (!Observe(result, "cell_morton_high_slab",
                    {{"morton_high_slab", mortonremap::MortonHighSlabLowerBound(cells)}}, error)) { return fail(error); }
            Unknown(result, EncodeStorageUnknown::MortonBucketDistribution);
        }
        if (descriptor.includeTopology && cells > 0u && !adapter.IsStructuredMesh()) {
            if (adapter.IsPolyhedronMesh()) {
                if (!Observe(result, "polyhedron_validation",
                        {{"polyhedron_visited_faces", adapter.GetNumberOfFaces()}}, error)) { return fail(error); }
                Unknown(result, EncodeStorageUnknown::TopologyRemapAndLocalIndexTable);
            } else {
                // Morton 对大于一的域生成非恒等的存储 provider，拓扑阶段复制两个连续数组并同时持有
                std::vector<EncodeStorageAllocation> allocations;
                std::size_t bytes = 0u;
                if (descriptor.pointOrder == EncodePointOrderMode::Morton) {
                    if (!topocodec::CalculateTopologyRemapStorageBytes(points, bytes, &error)) { return fail(error); }
                    allocations.push_back({"topology_inverse_point_remap", bytes});
                }
                if (descriptor.cellOrder == EncodeCellOrderMode::Morton) {
                    if (!topocodec::CalculateTopologyRemapStorageBytes(cells, bytes, &error)) { return fail(error); }
                    allocations.push_back({"topology_cell_order", bytes});
                }
                if (!Observe(result, "topology_remap_arrays", std::move(allocations), error)) { return fail(error); }
            }
        }
        if (points > 0u && !Regions(result, controls.geomControl, "geometry", error)) { return fail(error); }
        for (const auto& meta : metadata.attrParams) {
            if (stop.stop_requested()) { return fail({}); }
            // 含区域精度的字段走普通编码，引用选路不改变这份区域记录分配
            if (!Regions(result, controls.GetAttrControl(meta.name), meta.name, error)) { return fail(error); }
        }
        if (attributeRole == TemporalFieldRole::SingleFrame &&
            !Samples(result, metadata.attrParams, controls.attrReference, stop, error)) { return fail(error); }
        if (controls.attrReference.enabled || controls.geometryReference.enabled) {
            Unknown(result, EncodeStorageUnknown::ReferenceSelectionAndRetention);
        }
        if (stop.stop_requested()) { return fail({}); }
        result.inspectedLeafCount = 1u;
        result.success = true;
        return result;
    } catch (const std::exception& exception) {
        return fail(exception.what());
    } catch (...) {
        return fail("unknown encode storage analysis exception");
    }
}

void Merge(EncodeStorageAnalysisResult& result, EncodeStorageAnalysisResult leaf) {
    if (!leaf.success) { result = std::move(leaf); return; }
    result.inspectedLeafCount += leaf.inspectedLeafCount;
    for (const auto item : leaf.unresolved) { Unknown(result, item); }
    if (leaf.provenLowerBoundBytes.value_or(0u) > result.provenLowerBoundBytes.value_or(0u)) {
        result.provenLowerBoundBytes = leaf.provenLowerBoundBytes;
        result.peakStage = std::move(leaf.peakStage);
        result.peakAllocations = std::move(leaf.peakAllocations);
        result.peakFrameIndex = leaf.peakFrameIndex;
        result.peakLeafPath = std::move(leaf.peakLeafPath);
    }
}

}

namespace datacodec {
std::optional<CodecFailureRecord> CheckEncodeStorageLowerBound(
    const EncodeStorageAnalysisResult& analysis, const std::optional<std::uint64_t> limit) {
    if (!analysis.success || !analysis.provenLowerBoundBytes) {
        return analysis.failure ? analysis.failure : std::optional{MakeCodecFailureRecord(
            CodecErrorCode::InvalidInput, "encode.storage-analysis", "EncodeStorageAnalysis",
            "encode storage analysis did not complete")};
    }
    if (limit && *limit < *analysis.provenLowerBoundBytes) {
        auto failure = MakeCodecFailureRecord(CodecErrorCode::EncodeFailure, "encode.storage-limit-too-small",
            "EncodeStorageAnalysis", "fixed owned storage limit=" + std::to_string(*limit) +
            " bytes; proven necessary lower bound=" + std::to_string(*analysis.provenLowerBoundBytes) +
            " bytes; stage=" + analysis.peakStage + "; frame=" + std::to_string(analysis.peakFrameIndex) +
            "; leaf=" + analysis.peakLeafPath);
        failure.requestedBytes = analysis.provenLowerBoundBytes;
        failure.limitBytes = limit;
        return failure;
    }
    return std::nullopt;
}
}
