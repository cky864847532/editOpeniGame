#include "DataCodec/Storage/ByteIO/EncodedInputAccess.h"
#include "DataCodec/Workflow/Decode/DecodeStoragePlan.h"
#include "DataCodec/Codec/Attributes/AttributeDecodePlan.h"
#include "DataCodec/Runtime/Cache/DecodeCache/DecodedStorageSize.h"
#include "DataCodec/Runtime/Cache/DecodeCache/DecodedIndexCache.h"
#include "DataCodec/Storage/LeafPackage/LeafPackageIO.h"
#include "DataCodec/Storage/LeafPackage/LeafPackageFieldDecodeStream.h"
#include "DataCodec/Storage/Package/PackageBinaryHeader.h"
#include "DataCodec/Workflow/FrameSequence/FrameSequenceDependencyPlanner.h"
#include "DataCodec/API/Params/ParamsDecodeLimits.h"
#include "DataCodec/Codec/NumericArray/NumericDecodeMemoryPlan.h"
#include "DataCodec/Codec/Topology/Connectivity/ConnectivityDecodeMemoryPlan.h"
#include "DataCodec/Codec/Topology/Polyhedron/PolyhedronDecodeMemoryPlan.h"
#include "DataCodec/Codec/Topology/Polyhedron/PolyhedronTopologyStreamDecode.h"

#include <algorithm>
#include <numeric>
#include <stdexcept>

namespace datacodec::storageplan {
namespace {

struct Leaf {
    LeafPackage package;
    CodecStorageParams params;
    FramePackageLeafRecord record;
    std::vector<bool> completed, host;
    std::vector<std::uint64_t> newAttributeBytes;
    bool existing{false}, geometryReferenceReady{false}, topologyReady{false};
    std::uint64_t geometry{}, topology{}, geometryReference{};
};

struct Frame {
    FramePackage metadata;
    bool standalone{false};
    std::vector<Leaf> leaves;
};

// 分析专用有界计数游标，三条流各自保持外层解压状态
class CountCursor {
public:
    CountCursor(const LeafPackageField& field, const TopoStorageParams& topo, std::size_t ordinal,
        const CacheResources& resources, std::string& error) : stream(reader) {
        const bool opened = field.compressionType == EncodedFieldCompressionType::ZSTD
            ? reader.Open(field.source, field.rawSize, resources, &error, false)
            : reader.OpenRaw(field.source, field.rawSize, resources, &error, false);
        if (!opened) { throw std::runtime_error(error); }
        std::uint64_t offset = 0u;
        for (std::size_t i = 0u; i < ordinal; ++i) {
            if (!validation::CheckedAddU64(offset, topo.polyhedronStreamLayouts[i].encodedByteLength, offset,
                    "polyhedron count stream offset", &error)) { throw std::runtime_error(error); }
        }
        if (!stream.Skip(offset, &error)) { throw std::runtime_error(error); }
        schedule.streamByteSize = topo.polyhedronStreamLayouts.at(ordinal).encodedByteLength;
        schedule.elementCount = ordinal < 2u ? topo.cellCount : topo.polyhedronFaceVertexCount;
        bytes = std::make_unique<polyhedron::PolyhedronTopologyStreamByteReader<decodefield::FieldDecodeByteStream>>(
            stream, schedule, resources);
    }
    bool Read(std::size_t first, std::span<IndexType> output, std::string* error) {
        if (first != cursor || cursor > schedule.elementCount || output.size() > schedule.elementCount - cursor) {
            return validation::AssignError(error, "polyhedron analysis count range is invalid");
        }
        for (auto& value : output) {
            std::uint64_t decoded = 0u;
            if (!polyhedron::DecodeVarUInt64FromStream(*bytes, decoded, error) || decoded > std::numeric_limits<IndexType>::max()) {
                return false;
            }
            value = static_cast<IndexType>(decoded);
        }
        cursor += output.size();
        return cursor != schedule.elementCount || bytes->Finish(error);
    }
private:
    decodefield::FieldDecodeStreamReader reader;
    decodefield::FieldDecodeByteStream stream;
    polyhedron::PolyhedronTopologyStreamSchedule schedule;
    std::unique_ptr<polyhedron::PolyhedronTopologyStreamByteReader<decodefield::FieldDecodeByteStream>> bytes;
    std::size_t cursor{0u};
};

class Planner {
public:
    Planner(const DecodeStorageAnalysisRequest& request, const ExistingState& state)
        : request(request), state(state), parserRun(ResolvedResourceConfiguration{{0u, 1u, 1u}, 0u, 1u, false}),
          parserScope(parserRun) { parserResources.BindRun(parserRun); }

    DecodeStorageAnalysisResult Execute() {
        Cancel();
        const auto inputReader = EncodedInputAccess::Open(request.input);
        Check(inputReader != nullptr, "storage analysis requires an input reader");
        Check(request.attributeSelection == AttributeSelectionMode::None ||
            request.attributeSelection == AttributeSelectionMode::AllAvailable ||
            request.attributeSelection == AttributeSelectionMode::Explicit, "invalid attribute selection mode");
        Check(request.attributeSelection == AttributeSelectionMode::Explicit || request.attributeTargets.empty(),
            "explicit attribute targets require explicit selection mode");
        Cancel();
        PackageInspection inspection;
        Check(InspectPackage(*inputReader, inspection, &error, request.stopToken));
        std::vector<std::uint32_t> order;
        if (inspection.format == PackageBinaryFormat::LeafPackage) {
            targetFrame = request.frameIndex.value_or(0u);
            Frame frame;
            frame.standalone = true;
            frame.metadata.frameIndex = targetFrame;
            Leaf leaf;
            Check(LeafPackageIO::ReadFromByteRange(inputReader, 0u, inputReader->ByteSize(),
                leaf.package, &error, request.stopToken));
            leaf.record.path = leaf.package.path;
            ReadParams(leaf, targetFrame);
            frame.leaves.push_back(std::move(leaf));
            frames.emplace(targetFrame, std::move(frame));
            order.push_back(targetFrame);
        } else {
            Check(inspection.format == PackageBinaryFormat::FramePackage, "unsupported package for storage analysis");
            FrameSequenceDependencyPlanner::FrameReaderMap readers;
            FrameSequenceDependencyPlanner::FramePackageMap metadata;
            auto addSource = [&](const std::shared_ptr<IByteRangeReader>& reader) {
                Cancel();
                Check(reader != nullptr, "reference input reader is missing");
                auto frame = std::make_shared<FramePackage>();
                Check(FramePackageIO::ReadMetadata(*reader, *frame, &error, request.stopToken));
                const auto index = frame->frameIndex;
                Check(readers.emplace(index, reader).second, "duplicate frame index in storage analysis sources");
                metadata.emplace(index, std::move(frame));
                return index;
            };
            targetFrame = addSource(inputReader);
            Check(!request.frameIndex || *request.frameIndex == targetFrame, "requested frame does not match input");
            for (const auto& input : request.referenceInputs) { addSource(EncodedInputAccess::Open(input)); }
            FrameSequenceDependencyPlanner dependencies(readers, metadata);
            FrameSequenceDependencyPlan plan;
            Check(dependencies.BuildPlan(targetFrame, plan, &error));
            order = std::move(plan.decodeOrder);
            for (const auto index : order) {
                Frame frame;
                frame.metadata = *metadata.at(index);
                for (const auto& record : frame.metadata.leaves) {
                    Cancel();
                    Leaf leaf;
                    leaf.record = record;
                    Check(LeafPackageIO::ReadFromByteRange(readers.at(index), record.leafPackageByteOffset,
                        record.leafPackageByteSize, leaf.package, &error, request.stopToken));
                    leaf.package.path = record.path;
                    ReadParams(leaf, index);
                    frame.leaves.push_back(std::move(leaf));
                }
                frames.emplace(index, std::move(frame));
            }
        }
        ValidateTargets();
        Add(DecodeStorageKind::Existing, state.reservedBytes, "existing", targetFrame, {});
        for (const auto index : order) { PlanFrame(frames.at(index)); }
        result.success = true;
        result.minimumExecutionLimitBytes = peak;
        result.retainedOwnedStorageBytes = live;
        return std::move(result);
    }

private:
    void Cancel() const {
        if (request.stopToken.stop_requested()) { throw std::runtime_error("storage analysis cancelled"); }
    }
    void Check(bool ok, const char* detail = nullptr) {
        if (!ok) { throw std::runtime_error(detail ? detail : error.empty() ? "invalid decode storage plan" : error); }
    }
    static const LeafPackageField* Field(const Leaf& leaf, FieldType type) {
        for (const auto& field : leaf.package.fields) { if (field.type == type) { return &field; } }
        return nullptr;
    }
    std::uint64_t Sum(std::uint64_t a, std::uint64_t b) {
        std::uint64_t value = 0u;
        Check(validation::CheckedAddU64(a, b, value, "decode storage plan total", &error));
        return value;
    }
    void Add(DecodeStorageKind kind, std::uint64_t bytes, const char* stage, std::uint32_t frame, const BlockPath& path) {
        Cancel();
        live = Sum(live, bytes);
        auto& part = parts[static_cast<std::size_t>(kind)];
        part = Sum(part, bytes);
        if (live > peak) {
            peak = live;
            result.peakBytesByKind = parts;
            result.peakStage = stage;
            result.peakFrameIndex = frame;
            result.peakLeafPath = path;
            result.peakBlock = currentBlock;
        }
    }
    void Remove(DecodeStorageKind kind, std::uint64_t bytes) {
        auto& part = parts[static_cast<std::size_t>(kind)];
        Check(bytes <= live && bytes <= part, "decode storage lifetime is inconsistent");
        live -= bytes;
        part -= bytes;
    }
    void ReadParams(Leaf& leaf, std::uint32_t frame) {
        const auto* params = Field(leaf, FieldType::Params);
        Check(params != nullptr, "params field is missing");
        Check(params->rawSize <= kMaxDecodedParamsBytes, "params field exceeds decode limit");
        decodefield::FieldDecodeStreamReader reader;
        Check(decodefield::OpenLeafPackageFieldDecodeStream(*params, parserResources, reader, &error));
        decodefield::FieldDecodeByteStream stream(reader);
        std::vector<std::uint8_t> bytes;
        Check(stream.ReadVector(bytes, params->rawSize, &error));
        Check(DeserializeCodecStorageParams(bytes, leaf.params, &error));
        const auto count = leaf.params.attrParams.size();
        leaf.completed.assign(count, false);
        leaf.host.assign(count, false);
        leaf.newAttributeBytes.assign(count, 0u);
        for (const auto& prior : state.leaves) {
            if (prior.frameIndex != frame || prior.package.path != leaf.package.path) { continue; }
            Check(!prior.package.identity.IsValid() || prior.package.identity == leaf.package.identity,
                "existing workspace belongs to a different package revision");
            Check((prior.completeAttributes.empty() && prior.adapterBackedAttributes.empty()) ||
                (prior.completeAttributes.size() == count && prior.adapterBackedAttributes.size() == count),
                "existing attribute state does not match the package");
            leaf.existing = true;
            if (!prior.completeAttributes.empty()) {
                leaf.completed = prior.completeAttributes;
                leaf.host = prior.adapterBackedAttributes;
            }
            leaf.geometryReferenceReady = prior.geometryReferenceReady;
            leaf.topologyReady = prior.topologyReady;
            break;
        }
        const auto& meta = leaf.params.geomParams;
        if (meta.elementCount != 0u && meta.dimension != 0) {
            Check(Field(leaf, FieldType::Geometry) != nullptr, "geometry field is missing");
        }
        if (const auto* field = Field(leaf, FieldType::Topology)) {
            Check(field->rawSize == leaf.params.topoParams.binaryCount, "topology field size does not match params");
        }
        ++result.inspectedLeafCount;
    }
    void ValidateTargets() {
        if (request.attributeSelection != AttributeSelectionMode::Explicit) { return; }
        for (const auto& target : request.attributeTargets) {
            Check(target.frameIndex == targetFrame, "attribute target belongs to a different frame");
            auto& leaf = FindLeaf(target.frameIndex, target.blockPath);
            Check(target.attrIndex < leaf.params.attrParams.size(), "attribute target index is out of range");
        }
    }
    Leaf& FindLeaf(std::uint32_t frame, const BlockPath& path) {
        const auto found = frames.find(frame);
        Check(found != frames.end(), "required reference frame is unavailable");
        for (auto& leaf : found->second.leaves) { if (leaf.package.path == path) { return leaf; } }
        throw std::runtime_error("required reference leaf is unavailable");
    }
    std::uint64_t TopologyBytes(const Leaf& leaf) {
        const auto& topo = leaf.params.topoParams;
        if (!Field(leaf, FieldType::Topology) || topo.isStructured) { return 0u; }
        if (topo.isPolyhedron) {
            std::uint64_t total = 0u;
            for (const auto count : {topo.cellCount, topo.cellCount, topo.polyhedronFaceVertexCount,
                    topo.polyhedronVertexCount, topo.cellBufferSize}) {
                std::uint64_t bytes = 0u;
                Check(CalculateIndexCacheBytes(count, bytes, &error));
                total = Sum(total, bytes);
            }
            return total;
        }
        std::size_t cells = 0u, indices = 0u;
        Check(TryParamSizeToSizeT(topo.cellCount, cells) && TryParamSizeToSizeT(topo.cellBufferSize, indices),
            "topology exceeds local address space");
        const bool orders = std::any_of(topo.connectivityLayout.blockLayouts.begin(),
            topo.connectivityLayout.blockLayouts.end(), [](const auto& b) { return b.cellPolynomialOrderByteCount != 0u; });
        DecodedConnectivityStorageSize size;
        Check(CalculateDecodedConnectivityStorageSize(cells, indices, topo.fixedCellSize <= 0,
            topo.hasCellTypes != 0u, orders, size, &error));
        return Sum(Sum(size.connectivity, size.offsets), Sum(size.cellTypes, size.polynomialOrders));
    }
    std::uint64_t PayloadBytes(const Leaf& leaf) {
        const auto* field = Field(leaf, FieldType::Attribute);
        Check(field != nullptr, "attribute field is missing");
        std::uint64_t expected = 0u;
        for (const auto& attr : leaf.params.attrParams) { expected = Sum(expected, attr.binaryCount); }
        Check(field->rawSize == expected, "attribute field size does not match params");
        Check(field->compressionType == EncodedFieldCompressionType::None ||
            field->compressionType == EncodedFieldCompressionType::ZSTD, "unsupported field compression");
        return field->compressionType == EncodedFieldCompressionType::ZSTD ? field->rawSize : 0u;
    }
    void Work(DecodeStorageKind kind, std::uint64_t bytes, const char* stage, std::uint32_t frame,
        const Leaf& leaf, std::optional<std::uint64_t> block = {}) {
        currentBlock = block;
        Add(kind, bytes, stage, frame, leaf.package.path);
        Remove(kind, bytes);
        currentBlock.reset();
    }
    std::size_t FieldWindows(const Leaf& leaf, FieldType type) {
        const auto* field = Field(leaf, type);
        if (!field) { return 0u; }
        Check(field->compressionType == EncodedFieldCompressionType::None ||
            field->compressionType == EncodedFieldCompressionType::ZSTD, "unsupported field compression");
        return DecodeFieldWindowBytes(field->ByteSizeHint(), field->rawSize,
            field->compressionType == EncodedFieldCompressionType::ZSTD);
    }
    void NumericWork(Frame& frame, Leaf& leaf, const NumericArrayStorageParams& meta, bool geometry,
        const AttrStorageParams* targetAttribute = nullptr) {
        for (std::size_t i = 0u; i < meta.blockLayouts.size(); ++i) {
            const auto& block = meta.blockLayouts[i];
            const NumericArrayStorageParams* reference = nullptr;
            if (block.referenceKind == NumericArrayReferenceKind::IntraArray) {
                Check(block.localParentFieldIndex < leaf.params.attrParams.size(), "numeric reference index is invalid");
                reference = &leaf.params.attrParams[block.localParentFieldIndex];
            } else if (block.referenceKind == NumericArrayReferenceKind::TemporalKeyFrame) {
                auto& source = FindLeaf(geometry ? frame.metadata.geometryKeyFrameIndex : frame.metadata.attributeKeyFrameIndex,
                    leaf.package.path);
                if (geometry) { reference = &source.params.geomParams; }
                else {
                    for (const auto& attr : source.params.attrParams) {
                        if (targetAttribute && attr.name == targetAttribute->name && attr.attachmentType == targetAttribute->attachmentType &&
                            attr.dataType == meta.dataType && attr.dimension == meta.dimension) { reference = &attr; break; }
                    }
                }
            }
            const auto memory = numericarray::MakeNumericDecodeMemoryLayout(meta, block, reference, false, true);
            Work(DecodeStorageKind::BlockWork, memory.TotalBytes(), geometry ? "geometry-block" : "attribute-block",
                frame.metadata.frameIndex, leaf, i);
        }
    }
    void TopologyWork(std::uint32_t frame, Leaf& leaf) {
        const auto& topo = leaf.params.topoParams;
        if (!Field(leaf, FieldType::Topology) || topo.isStructured) { return; }
        const auto windows = FieldWindows(leaf, FieldType::Topology);
        Add(DecodeStorageKind::StageWork, windows, "topology-field", frame, leaf.package.path);
        if (topo.isPolyhedron) {
            Work(DecodeStorageKind::StageWork, polyhedron::PolyhedronSequentialWorkBytes(topo), "polyhedron-streams", frame, leaf);
        } else {
            for (std::size_t i = 0u; i < topo.connectivityLayout.blockLayouts.size(); ++i) {
                const auto layout = topocodec::MakeConnectivityDecodeMemoryLayout(topo.connectivityLayout.blockLayouts[i],
                    static_cast<int>(topo.fixedCellSize), topo.hasCellTypes != 0u);
                Work(DecodeStorageKind::BlockWork, layout.TotalBytes(), "topology-block", frame, leaf, i);
            }
        }
        Remove(DecodeStorageKind::StageWork, windows);
    }
    void PlanAttributes(Frame& frame, Leaf& leaf, const std::vector<std::size_t>& targets, bool referenceHelper = false) {
        if (targets.empty()) { return; }
        std::vector<std::size_t> order;
        Check(decodeimpl::detail::ResolveAttributeExecutionOrder(leaf.params, targets, order, &error));
        if (std::all_of(targets.begin(), targets.end(), [&](auto index) { return leaf.completed[index]; })) { return; }
        const auto payload = PayloadBytes(leaf);
        const auto frameIndex = frame.metadata.frameIndex;
        Add(DecodeStorageKind::FieldPayload, payload, referenceHelper ? "reference-payload" : "attribute-payload",
            frameIndex, leaf.package.path);
        if (Field(leaf, FieldType::Attribute)->compressionType == EncodedFieldCompressionType::ZSTD) {
            Work(DecodeStorageKind::StageWork, FieldWindows(leaf, FieldType::Attribute), "attribute-expand", frameIndex, leaf);
        }
        for (const auto index : order) {
            Cancel();
            if (leaf.completed[index]) { continue; }
            const auto& attr = leaf.params.attrParams[index];
            const bool temporal = std::any_of(attr.blockLayouts.begin(), attr.blockLayouts.end(),
                [](const auto& b) { return b.referenceKind == NumericArrayReferenceKind::TemporalKeyFrame; });
            if (temporal) {
                Check(!referenceHelper && frame.metadata.attributeTemporalRole != TemporalFieldRole::SingleFrame &&
                    frame.metadata.attributeKeyFrameIndex != frameIndex, "attribute temporal reference is unavailable");
                auto& referenceFrame = frames.at(frame.metadata.attributeKeyFrameIndex);
                auto& reference = FindLeaf(referenceFrame.metadata.frameIndex, leaf.package.path);
                const auto& attrs = reference.params.attrParams;
                const auto match = std::find_if(attrs.begin(), attrs.end(), [&](const auto& candidate) {
                    return candidate.name == attr.name && candidate.attachmentType == attr.attachmentType &&
                        candidate.dataType == attr.dataType && candidate.dimension == attr.dimension;
                });
                Check(match != attrs.end(), "reference attribute metadata does not match target");
                const auto referenceIndex = static_cast<std::size_t>(match - attrs.begin());
                if (!reference.completed[referenceIndex]) {
                    PlanAttributes(referenceFrame, reference, {referenceIndex}, true);
                }
            }
            if (!leaf.host[index]) {
                std::uint64_t bytes = 0u;
                Check(CalculateDecodedNumericStorageBytes(attr, bytes, &error));
                leaf.newAttributeBytes[index] = bytes;
                Add(DecodeStorageKind::Attributes, bytes, "attribute", frameIndex, leaf.package.path);
            }
            NumericWork(frame, leaf, attr, false, &attr);
            leaf.completed[index] = true;
        }
        Remove(DecodeStorageKind::FieldPayload, payload);
    }
    void PlanFrame(Frame& frame) {
        const auto index = frame.metadata.frameIndex;
        for (auto& leaf : frame.leaves) {
            Cancel();
            if (leaf.existing && index != targetFrame) { continue; }
            const bool supplement = index == targetFrame && state.supplementAttributesOnly;
            Check(!supplement || leaf.existing, "attribute supplement requires an existing leaf workspace");
            if (!supplement) {
                const auto& meta = leaf.params.geomParams;
                Check(CalculateGeometryCacheBytes(meta.elementCount, static_cast<std::size_t>(meta.dimension), meta.dataType, leaf.geometry, &error));
                Add(DecodeStorageKind::Geometry, leaf.geometry, "geometry", index, leaf.package.path);
                if (frame.metadata.geometryTemporalRole != TemporalFieldRole::SingleFrame &&
                    frame.metadata.geometryKeyFrameIndex == index && meta.elementCount != 0u && meta.dimension != 0) {
                    Check(CalculateDecodedNumericStorageBytes(meta, leaf.geometryReference, &error));
                    Add(DecodeStorageKind::GeometryReference, leaf.geometryReference, "geometry-reference", index, leaf.package.path);
                    leaf.geometryReferenceReady = true;
                }
                if (std::any_of(meta.blockLayouts.begin(), meta.blockLayouts.end(),
                        [](const auto& b) { return b.referenceKind == NumericArrayReferenceKind::TemporalKeyFrame; })) {
                    Check(frame.metadata.geometryKeyFrameIndex != index &&
                        FindLeaf(frame.metadata.geometryKeyFrameIndex, leaf.package.path).geometryReferenceReady,
                        "geometry key frame reference is unavailable");
                }
                const auto geometryWindows = FieldWindows(leaf, FieldType::Geometry);
                Add(DecodeStorageKind::StageWork, geometryWindows, "geometry-field", index, leaf.package.path);
                NumericWork(frame, leaf, meta, true);
                Remove(DecodeStorageKind::StageWork, geometryWindows);
                if (leaf.record.topologyMode == TopologyOwnershipMode::Reused &&
                    leaf.params.topoParams.cellCount != 0u && !leaf.params.topoParams.isStructured) {
                    Check(FindLeaf(leaf.record.ownerFrameIndex, leaf.package.path).topologyReady,
                        "topology reference is unavailable");
                } else {
                    Check(Field(leaf, FieldType::Topology) || leaf.params.topoParams.isStructured ||
                        (leaf.params.meshType == MeshType::PointSet && leaf.params.topoParams.cellCount == 0u &&
                            leaf.params.topoParams.cellBufferSize == 0u), "owned topology field is missing");
                    leaf.topology = TopologyBytes(leaf);
                    Add(DecodeStorageKind::Topology, leaf.topology, "topology", index, leaf.package.path);
                    TopologyWork(index, leaf);
                }
                leaf.topologyReady = true;
            }
            // 当前 PlaybackSession 对依赖帧执行全部属性解码
            const auto selection = index == targetFrame ? request.attributeSelection : AttributeSelectionMode::AllAvailable;
            auto targets = ResolveAttributeDecodeIndices(request.attributeTargets, index, leaf.package.path,
                selection, leaf.params.attrParams.size());
            PlanAttributes(frame, leaf, targets);
            if (!supplement) {
                Remove(DecodeStorageKind::Geometry, leaf.geometry);
                leaf.geometry = 0u;

                if (frame.standalone) {
                    Remove(DecodeStorageKind::Topology, leaf.topology);
                    leaf.topology = 0u;
                }
            }
        }
        if (index == targetFrame) { return; }
        for (auto& leaf : frame.leaves) {
            if (leaf.existing) { continue; }
            // 任一发布的 reference 都通过共享 workspace 保留该叶的属性 owner
            const bool retainsWorkspace = leaf.geometryReferenceReady ||
                (frame.metadata.attributeTemporalRole != TemporalFieldRole::SingleFrame &&
                    frame.metadata.attributeKeyFrameIndex == index) ||
                leaf.record.topologyMode == TopologyOwnershipMode::Owned;
            if (!retainsWorkspace) {
                for (auto& bytes : leaf.newAttributeBytes) { Remove(DecodeStorageKind::Attributes, bytes); bytes = 0u; }
            }
        }
    }

    const DecodeStorageAnalysisRequest& request;
    const ExistingState& state;
    DataCodecExecutionResources parserRun;
    CodecRunScope parserScope;
    CacheResources parserResources;
    std::string error;
    std::map<std::uint32_t, Frame> frames;
    std::uint32_t targetFrame{};
    std::uint64_t live{}, peak{};
    std::optional<std::uint64_t> currentBlock;
    std::array<std::uint64_t, kDecodeStorageKindCount> parts{};
    DecodeStorageAnalysisResult result;
};

}

DecodeStorageAnalysisResult Analyze(const DecodeStorageAnalysisRequest& request, const ExistingState& state) {
    try { return Planner(request, state).Execute(); }
    catch (const std::exception& exception) {
        DecodeStorageAnalysisResult result;
        result.cancelled = request.stopToken.stop_requested();
        if (!result.cancelled) {
            result.failure = MakeCodecFailureRecord(CodecErrorCode::DecodeFailure,
                "decode-storage-analysis", "AnalyzeDecodeStorage", exception.what());
        }
        return result;
    } catch (...) {
        DecodeStorageAnalysisResult result;
        result.failure = MakeCodecFailureRecord(CodecErrorCode::DecodeFailure,
            "decode-storage-analysis", "AnalyzeDecodeStorage", "unexpected storage analysis exception");
        return result;
    }
}

}
