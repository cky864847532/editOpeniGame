#ifndef DATACODEC_TEST_FEATURE_DATACODECFEATUREPIPELINECONTRACTS_H
#define DATACODEC_TEST_FEATURE_DATACODECFEATUREPIPELINECONTRACTS_H

#include "DataCodec/Codec/NumericArray/SpatialBlockLayout.h"
#include "DataCodec/Codec/Remap/Common/MortonRemapBuilder.h"
#include "DataCodec/Runtime/Cache/TransferCache/ReferenceTransferCacheBuilder.h"
#include "DataCodec/Codec/Reference/AttributeReferenceScheduleBuilder.h"
#include "DataCodec/Codec/Topology/TopologyFingerprint.h"
#include "DataCodec/Runtime/Cache/CacheResources.h"
#include "DataCodec/API/Params/CodecParamDefaults.h"
#include "DataCodec/API/Entry/DataCodecEncodeEntry.h"
#include "DataCodec/API/Entry/DataCodecDecodeEntry.h"
#include "DataCodec/Workflow/Session/PlaybackSession.h"
#include "DataCodec/Workflow/Encode/EncodePipelineBinding.h"
#include "DataCodec/Workflow/Encode/EncodePipeline.h"
#include "DataCodec/Runtime/Context/EncodeContext.h"
#include "DataCodec/Workflow/Leaf/LeafEncodeExecutor.h"
#include "DataCodec/Workflow/Session/EncodeSessionWorkspace.h"
#include "DataCodec/Workflow/Temporal/TemporalBuilder.h"
#include "DataCodec/Workflow/FrameSequence/FrameSequenceEncodeExecutor.h"
#include "DataCodec/Workflow/FrameSequence/FrameSequenceDependencyPlanner.h"
#include "DataCodec/Test/Adapter/DataCodecTestAdapter.h"
#include "DataCodec/Test/Common/DataCodecTestResult.h"

#include <algorithm>
#include <array>
#include <string>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <memory>
#include <span>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace datacodec::test {

static_assert(std::is_same_v<decltype(EncodeRequest::resources), CodecResourceParams>);
static_assert(std::is_same_v<decltype(DecodePackageRequest::resources), CodecResourceParams>);
static_assert(std::is_same_v<decltype(PlaybackOpenRequest::resources), CodecResourceParams>);
static_assert(std::is_same_v<decltype(PlaybackSequenceOpenRequest::resources), CodecResourceParams>);

inline bool HasPipelineStageName(
    const std::vector<EncodeStageId>& stageIds,
    const std::string_view name) {
    return std::any_of(
        stageIds.begin(),
        stageIds.end(),
        [name](const EncodeStageId& stageId) {
            return stageId.name == name;
        });
}

inline bool HasPipelineStagePrefix(
    const std::vector<EncodeStageId>& stageIds,
    const std::string_view prefix) {
    return std::any_of(
        stageIds.begin(),
        stageIds.end(),
        [prefix](const EncodeStageId& stageId) {
            return stageId.name.starts_with(prefix);
        });
}

inline bool CheckResolvedFormalPipeline(
    TestResult& result,
    std::string* error = nullptr) {
    auto dataset = MakePipelineContractUnstructuredDataset();
    TestEncodeAdapter adapter(dataset);
    auto params = CodecControlParamsFactory::MakeDefault();
    const std::vector<AttributeTarget> targets{
        AttributeTarget{.frameIndex = 0u, .blockPath = {}, .attrIndex = 0u},
        AttributeTarget{.frameIndex = 0u, .blockPath = {}, .attrIndex = 1u},
        AttributeTarget{.frameIndex = 0u, .blockPath = {}, .attrIndex = 2u},
    };
    DataCodecExecutionResources resources(CodecResourceParams{.mode = CodecResourceMode::Fixed, .maxComputeThreads = 1u});
    CodecRunScope run(resources);
    EncodeContext context(resources);
    context.adapter = &adapter;
    context.controlParams = &params;
    context.frameIndex = 0u;
    context.path = {};
    context.attributeTargets = std::span<const AttributeTarget>(
        targets.data(),
        targets.size());
    auto controls = CodecControlParamsFactory::MakeEncodeConfiguration(
        DataCodecEncodeOptions{});
    // 本用例显式选择双 Morton 流程以验证 Point 到 Cell 的阶段依赖
    controls.pipelineControl.cellOrder = EncodeCellOrderMode::Morton;
    EncodePipelineBinding binding;
    if (!ResolveEncodePipelineBinding(
        adapter,
        params,
        controls.pipelineControl,
        binding,
        error,
        EncodePipelineOutputKind::LeafPackage)) {
        return false;
    }
    EncodePipeline pipeline(EncodePipelineOptions{
        .binding = std::move(binding),
    });
    const auto stageGraph = pipeline.DescribeStageGraph(context);
    const auto stageIds = pipeline.DescribeStageIds(context);
    if (context.HasFailure()) {
        return validation::AssignError(
            error,
            context.HasFailure()
                ? FormatCodecFailure(*context.FirstFailure())
                : "formal pipeline description failed");
    }

    const auto hasFormalStages =
        HasPipelineStagePrefix(stageIds, "PointSpatialPartition.Morton") &&
        HasPipelineStagePrefix(stageIds, "CellSpatialPartition.Morton") &&
        HasPipelineStageName(stageIds, "GeometryStage") &&
        HasPipelineStageName(stageIds, "TopoStage") &&
        HasPipelineStageName(stageIds, "PointAttributeStage") &&
        HasPipelineStageName(stageIds, "CellAttributeStage") &&
        HasPipelineStageName(stageIds, "ParamsEncodeStage") &&
        HasPipelineStagePrefix(stageIds, "ReferenceEncode.AttributeIntra.AffineSpatialBlock") &&
        HasPipelineStageName(stageIds, "PackageFieldZstd.Streaming") &&
        HasPipelineStageName(stageIds, "PackageAssembly.LeafPackage");
    const auto formalStagesOk = Require(
        result,
        hasFormalStages,
        "pipeline.resolvedFormalStages",
        "resolved unstructured pipeline is missing a required concrete stage variant");
    const auto pointIt = std::find_if(
        stageGraph.begin(),
        stageGraph.end(),
        [](const EncodeStageDescription& description) {
            return description.stageId.name.starts_with("PointSpatialPartition.Morton");
        });
    const auto cellIt = std::find_if(
        stageGraph.begin(),
        stageGraph.end(),
        [](const EncodeStageDescription& description) {
            return description.stageId.name.starts_with("CellSpatialPartition.Morton");
        });
    const auto hasHardDependency =
        pointIt != stageGraph.end() &&
        cellIt != stageGraph.end() &&
        std::find(
            cellIt->dependencies.begin(),
            cellIt->dependencies.end(),
            pointIt->stageId) != cellIt->dependencies.end();
    const auto dependencyOk = Require(
        result,
        hasHardDependency,
        "pipeline.pointBeforeCellDependency",
        "cell spatial partition has no explicit point spatial partition dependency");
    return formalStagesOk && dependencyOk;
}

inline bool CheckTopologyReuseSpatialDependency(TestResult& result) {
    auto ownerDataset = MakePipelineContractUnstructuredDataset();
    auto currentDataset = ownerDataset;
    currentDataset.points[0] += 0.5f;
    TestEncodeAdapter ownerAdapter(ownerDataset);
    TestEncodeAdapter currentAdapter(currentDataset);
    TopologyFingerprint ownerTopology;
    TopologyFingerprint currentTopology;
    std::string fingerprintError;
    const auto fingerprintsBuilt =
        TopologyFingerprintBuilder::Build(ownerAdapter, ownerTopology, &fingerprintError) &&
        TopologyFingerprintBuilder::Build(currentAdapter, currentTopology, &fingerprintError);
    const auto ownerSpatial = PointSpatialFingerprintBuilder::Build(ownerAdapter);
    const auto currentSpatial = PointSpatialFingerprintBuilder::Build(currentAdapter);
    return Require(
        result,
        fingerprintsBuilt && ownerTopology == currentTopology && !(ownerSpatial == currentSpatial),
        "pipeline.topologyReuseSpatialDependency",
        fingerprintError.empty()
            ? "topology reuse cannot distinguish changed point spatial partition input"
            : fingerprintError);
}

inline void CheckDecodeSessionOwnerLifetime(TestResult& result, EncodedBuffer encodedBytes) {
    constexpr std::uint64_t limit = 32u * 1024u * 1024u;
    DataCodecExecutionResources root(ResolvedResourceConfiguration{{limit, 1u, 1u},
        limit, 1u, false, true, false});
    CodecRunScope scope(root);
    auto reader = std::make_shared<MemoryByteRangeReader>(
        std::make_shared<const EncodedBuffer>(std::move(encodedBytes)));
    LeafPackage leaf;
    std::string error;
    if (!Require(result, LeafPackageIO::ReadFromByteRange(reader, 0u, reader->ByteSize(), leaf, &error),
            "pipeline.decode-session-input", error)) { return; }
    class Assembly final : public IFramePackageDecodeAssembly {
    public:
        bool BeginFramePackage(const FramePackage&, std::string*) override { return true; }
        bool AddBranch(const FramePackageBranchRecord&, std::string*) override { return true; }
        std::unique_ptr<IDecodeAdapter> CreateLeafAdapter(const FramePackageLeafRecord&, const LeafPackage&, std::string*) override {
            return std::make_unique<TestDecodeAdapter>();
        }
        bool CommitLeaf(const FramePackageLeafRecord&, IDecodeAdapter&, std::string*) override { ++commits; return true; }
        bool EndFramePackage(std::string*) override { return true; }
        std::size_t commits{0u};
    } assembly;
    const DecodeReferenceKey key{.source = {"session-owner-fixture", "1"}, .keyFrameIndex = 0u};
    auto identities = std::make_shared<DecodeSession::FrameIdentityMap>();
    identities->emplace(0u, key.source);
    auto cache = root.Caches().ReferenceCache();
    cache->Configure(0u);
    auto required = cache->RequireFrame(key);
    std::shared_ptr<DecodedAttributeCacheSet> attributeOwner;
    std::shared_ptr<bytestore::ByteStoreSession> sessionOwner;
    std::shared_ptr<DecodedGeometryReferenceCache> geometryOwner;
    std::weak_ptr<DecodedAttributeCacheSet> weakWorkspace;
    std::vector<float> expectedGeometry;
    {
        DecodeSession session;
        session.ConfigureReferences(root, identities);
        FramePackage frame;
        frame.geometryTemporalRole = TemporalFieldRole::KeyFrame;
        frame.attributeTemporalRole = TemporalFieldRole::KeyFrame;
        FramePackageLeafRecord record{.path = leaf.path, .name = "owner"};
        frame.leaves.push_back(record);
        const bool begun = session.BeginFramePackage(frame, assembly, 0u, &error);
        auto adapter = begun ? session.CreateLeafAdapter(record, leaf, &error) : nullptr;
        const auto decoded = session.DecodeLeaf(LeafDecodeRequest{.adapter = adapter.get(), .leafPackage = &leaf,
            .attributeSelection = AttributeSelectionMode::AllAvailable,
            .attributeRequestMode = AttributeDecodeRequestMode::DecodeToCache, .resources = &root});
        const bool complete = decoded.success && session.CommitLeaf(record, *adapter, &error) && session.EndFramePackage(&error);
        if (!Require(result, complete && session.LeafStateCount() == 1u && assembly.commits == 1u,
                "pipeline.decode-session-publish", decoded.failure ? FormatCodecFailure(*decoded.failure) : error)) { return; }
        expectedGeometry = static_cast<TestDecodeAdapter*>(adapter.get())->Points();
        auto published = cache->Find(key);
        if (!Require(result, published && published->leaves.contains(leaf.path) &&
                published->leaves.at(leaf.path).attribute && published->leaves.at(leaf.path).geometry,
                "pipeline.decode-session-reference", "real session must publish geometry and aliasing attribute owners")) { return; }
        attributeOwner = published->leaves.at(leaf.path).attribute->store;
        sessionOwner = published->leaves.at(leaf.path).attribute->byteStoreSession;
        geometryOwner = published->leaves.at(leaf.path).geometry->store;
        weakWorkspace = attributeOwner;
        FramePackage nextFrame;
        nextFrame.frameIndex = 1u;
        Require(result, session.BeginFramePackage(nextFrame, assembly, 1u, &error) && session.EndFramePackage(&error) &&
            session.LeafStateCount() == 0u && session.RetainedFrameIndices().empty() && !weakWorkspace.expired(),
            "pipeline.decode-session-prunes-predecessor", "advancing to an independent frame must retire the prior leaf state while required consumers retain its workspace");
    }
    required.Reset();
    Require(result, !cache->Find(key) && !weakWorkspace.expired() && root.StorageCapacity()->Snapshot().reservedBytes != 0u,
        "pipeline.decode-session-command-retired", "required-frame retirement and session destruction must preserve external consumers without cache retention");
    std::vector<float> geometry(expectedGeometry.size());
    float attribute = 0.0f;
    Require(result, geometryOwner && geometryOwner->ReadRange(0u, geometry.size() / 3u, geometry.data(),
            geometry.size() * sizeof(float), &error) && geometry == expectedGeometry && attributeOwner &&
        attributeOwner->ReadRange(0u, 0u, 1u, &attribute, sizeof(attribute), &error) && std::isfinite(attribute),
        "pipeline.decode-session-surviving-read", "real reference data must remain readable after predecessor, session and command retirement");
    geometryOwner.reset();
    attributeOwner.reset();
    Require(result, !weakWorkspace.expired() && root.StorageCapacity()->Snapshot().reservedBytes != 0u,
        "pipeline.decode-session-alias-retains-workspace", "the aliasing byte-store session must retain the actual workspace and its data");
    sessionOwner.reset();
    Require(result, weakWorkspace.expired() &&
        root.StorageCapacity()->Snapshot().reservedBytes == root.Scratch().RetainedFixedBytes() &&
        scope.Finish(true) && root.StorageCapacity()->Snapshot().reservedBytes == 0u,
        "pipeline.decode-session-final-owner", "the last real workspace alias must release all decoded storage capacity");
}

inline void CheckLargePlanningMetadata(TestResult& result) {
    constexpr std::uint32_t frameCount = 4096u;
    FrameSequenceDependencyPlanner::FramePackageMap packages;
    FrameSequenceDependencyPlanner::FrameReaderMap readers;
    class UnreadReader final : public IByteRangeReader {
    public:
        std::uint64_t ByteSize() const noexcept override { return 0u; }
        bool ReadAt(std::uint64_t, std::span<std::uint8_t>, std::string*) override { ++reads; return false; }
        mutable std::size_t reads{0u};
    };
    auto reader = std::make_shared<UnreadReader>();
    std::uint64_t knownMetadataBodies = 0u;
    for (std::uint32_t i = 0u; i < frameCount; ++i) {
        auto frame = std::make_shared<FramePackage>();
        frame->frameIndex = i;
        frame->leaves.resize(16u);
        knownMetadataBodies += sizeof(FramePackage) + frame->leaves.capacity() * sizeof(FramePackageLeafRecord);
        packages.emplace(i, std::move(frame));
        readers.emplace(i, reader);
    }
    auto target = std::make_shared<FramePackage>();
    target->frameIndex = frameCount;
    for (std::uint32_t i = 0u; i < frameCount; ++i) {
        target->leaves.push_back(FramePackageLeafRecord{.ownerFrameIndex = i, .topologyMode = TopologyOwnershipMode::Reused});
    }
    knownMetadataBodies += sizeof(FramePackage) + target->leaves.capacity() * sizeof(FramePackageLeafRecord);
    packages.emplace(frameCount, target);
    readers.emplace(frameCount, reader);
    std::weak_ptr<const FramePackage> weak = packages.at(0u);
    FrameSequenceDependencyPlan plan;
    std::string error;
    double elapsedMs = 0.0;
    {
        FrameSequenceDependencyPlanner planner(readers, packages);
        bool shared = true;
        for (std::uint32_t i = 0u; i < frameCount; ++i) { shared &= packages.at(i).use_count() == 2u; }
        packages.clear();
        readers.clear();
        const auto begin = std::chrono::steady_clock::now();
        bool planned = planner.BuildPlan(frameCount, plan, &error);
        elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        planned &= plan.decodeOrder.size() == frameCount + 1u && plan.referenceFrames.size() == frameCount;
        for (std::size_t i = 0u; planned && i < plan.decodeOrder.size(); ++i) { planned &= plan.decodeOrder[i] == i; }
        for (std::size_t i = 0u; planned && i < plan.referenceFrames.size(); ++i) { planned &= plan.referenceFrames[i] == i; }
        Require(result, planned && shared && !weak.expired() && reader->reads == 0u,
            "pipeline.large-planning-shares-metadata", error.empty() ?
                "large preloaded planning must preserve dependency order and share metadata and readers without reparsing" : error);
    }
    Require(result, weak.expired() && reader.use_count() == 1u,
        "pipeline.large-planning-release", "planner destruction must release its shared metadata and reader map");
    result.AddDiagnostic("planning_metadata frames=" + std::to_string(frameCount + 1u) +
        " leaves=" + std::to_string(frameCount * 17u) + " known_object_and_leaf_array_bytes=" + std::to_string(knownMetadataBodies) +
        " result_array_capacity_bytes=" + std::to_string((plan.decodeOrder.capacity() + plan.referenceFrames.capacity()) * sizeof(std::uint32_t)) +
        " planning_ms=" + std::to_string(elapsedMs) + " map_nodes_strings_control_blocks_and_allocator_overhead=unmeasured");
}

inline bool CheckFormalPipelineExecution(
    TestResult& result,
    std::string* error = nullptr) {
    auto dataset = MakePipelineContractUnstructuredDataset();
    TestEncodeAdapter adapter(dataset);
    auto params = CodecControlParamsFactory::MakeDefault();
    const std::vector<AttributeTarget> targets{
        AttributeTarget{.frameIndex = 0u, .blockPath = {}, .attrIndex = 0u},
        AttributeTarget{.frameIndex = 0u, .blockPath = {}, .attrIndex = 1u},
        AttributeTarget{.frameIndex = 0u, .blockPath = {}, .attrIndex = 2u},
    };
    DataCodecExecutionResources resources(CodecResourceParams{.mode = CodecResourceMode::Fixed, .maxComputeThreads = 1u});
    CodecRunScope run(resources);
    EncodeContext context(resources);
    context.adapter = &adapter;
    context.controlParams = &params;
    context.objectName = dataset.name;
    context.meshType = "UnstructuredMesh";
    context.frameIndex = 0u;
    context.path = {};
    context.attributeTargets = std::span<const AttributeTarget>(
        targets.data(),
        targets.size());
    auto encoded = LeafEncodeExecutor::Execute(LeafEncodeRequest{
        .context = &context,
    });
    if (!encoded.success || !encoded.hasEncodedOutput || encoded.encodedBytes.empty()) {
        if (error != nullptr) {
            *error = context.HasFailure()
                ? FormatCodecFailure(*context.FirstFailure())
                : "formal unstructured pipeline did not produce encoded output";
        }
        return false;
    }
    const auto allCompleted = std::all_of(
        encoded.stageExecutions.begin(),
        encoded.stageExecutions.end(),
        [](const EncodeStageExecutionRecord& record) {
            return record.status == EncodeStageExecutionStatus::Completed ||
                record.status == EncodeStageExecutionStatus::EmptyInput;
        });
    const auto hasPoint = std::any_of(
        encoded.stageExecutions.begin(),
        encoded.stageExecutions.end(),
        [](const EncodeStageExecutionRecord& record) {
            return record.stageId.name.starts_with("PointSpatialPartition.Morton");
        });
    const auto hasCell = std::any_of(
        encoded.stageExecutions.begin(),
        encoded.stageExecutions.end(),
        [](const EncodeStageExecutionRecord& record) {
            return record.stageId.name.starts_with("CellSpatialPartition.Morton");
        });
    const auto hasZstd = std::any_of(
        encoded.stageExecutions.begin(),
        encoded.stageExecutions.end(),
        [](const EncodeStageExecutionRecord& record) {
            return record.stageId.name == "PackageFieldZstd.Streaming";
        });
    const auto hasAssembly = std::any_of(
        encoded.stageExecutions.begin(),
        encoded.stageExecutions.end(),
        [](const EncodeStageExecutionRecord& record) {
            return record.stageId.name == "PackageAssembly.LeafPackage";
        });
    const auto zstdPipelineOk = Require(
        result,
        allCompleted && hasPoint && hasCell && hasZstd && hasAssembly,
        "pipeline.formalExecution",
        "formal unstructured pipeline did not complete every required stage");

    EncodeContext rawContext(resources);
    rawContext.adapter = &adapter;
    rawContext.controlParams = &params;
    rawContext.objectName = dataset.name;
    rawContext.meshType = "UnstructuredMesh";
    rawContext.frameIndex = 0u;
    rawContext.path = {};
    rawContext.attributeTargets = std::span<const AttributeTarget>(
        targets.data(),
        targets.size());
    auto rawControl = EncodePipelineControlParams{};
    rawControl.packageFields.mode = PackageFieldEncodingMode::Raw;
    const auto rawEncoded = LeafEncodeExecutor::Execute(LeafEncodeRequest{
        .context = &rawContext,
        .pipelineControl = rawControl,
    });
    const auto hasRaw = std::any_of(
        rawEncoded.stageExecutions.begin(),
        rawEncoded.stageExecutions.end(),
        [](const EncodeStageExecutionRecord& record) {
            return record.stageId.name == "PackageFieldRaw.Streaming";
        });
    const auto rawPipelineOk = Require(
        result,
        rawEncoded.success && rawEncoded.hasEncodedOutput && hasRaw,
        "pipeline.rawExecution",
        "raw package pipeline did not execute the explicit raw field stage");
    CheckDecodeSessionOwnerLifetime(result, std::move(encoded.encodedBytes));
    return zstdPipelineOk && rawPipelineOk;
}

inline bool CheckTemporalPipelineExecution(
    TestResult& result,
    std::string* error = nullptr) {
    auto keyFrameDataset = MakePipelineContractUnstructuredDataset();
    auto predictedDataset = keyFrameDataset;
    predictedDataset.name = "pipeline_contract_predicted";
    for (auto& field : predictedDataset.pointFields) {
        for (auto& value : field.values) {
            value = value * 1.01f + 0.001f;
        }
    }
    for (auto& field : predictedDataset.cellFields) {
        for (auto& value : field.values) {
            value += 0.01f;
        }
    }
    TestBlockTreeAdapter keyFrameAdapter(keyFrameDataset);
    TestBlockTreeAdapter predictedAdapter(predictedDataset);
    auto params = CodecControlParamsFactory::MakeDefault();
    params.attrReference.temporalField.selectionMode = ReferenceSelectionMode::Forced;
    params.geometryReference.temporalField.selectionMode = ReferenceSelectionMode::Forced;
    class TemporalSequenceSource final : public IFrameSequenceEncodeSource {
    public:
        TemporalSequenceSource(
            const TestDataset& keyFrame,
            const TestDataset& predictedFrame)
            : m_frames{&keyFrame, &predictedFrame} {}

        [[nodiscard]] std::size_t FrameCount() const noexcept override {
            return m_frames.size();
        }

        bool LoadFrame(
            const std::size_t frameOrdinal,
            FrameSequenceEncodeFrame& frame,
            std::string* error) override {
            if (frameOrdinal >= m_frames.size()) {
                return validation::AssignError(error, "temporal test frame ordinal is invalid");
            }
            const auto& dataset = *m_frames[frameOrdinal];
            frame = {};
            frame.blockTreeAdapter = std::make_unique<TestBlockTreeAdapter>(dataset);
            frame.rootName = dataset.name;
            frame.frameIndex = static_cast<std::uint32_t>(frameOrdinal);
            frame.timeValue = static_cast<float>(frameOrdinal);
            for (std::size_t attrIndex = 0u; attrIndex < 3u; ++attrIndex) {
                frame.attributeTargets.push_back(AttributeTarget{
                    .frameIndex = frame.frameIndex,
                    .blockPath = "leaf",
                    .attrIndex = attrIndex,
                });
            }
            return true;
        }

    private:
        std::array<const TestDataset*, 2u> m_frames;
    };

    class TemporalSequenceOutput final : public IFrameSequenceOutputSink {
    public:
        [[nodiscard]] std::unique_ptr<IByteRangeOutput> OpenFrame(
            std::size_t,
            std::uint32_t,
            std::string*) override {
            return std::make_unique<MemoryByteRangeOutput>(m_outputRoot);
        }

        bool CommitFrame(
            std::size_t,
            std::uint32_t,
            const std::uint64_t encodedByteCount,
            std::string* error) override {
            if (encodedByteCount == 0u) {
                return validation::AssignError(error, "temporal test frame output is empty");
            }
            m_encodedBytes += encodedByteCount;
            return true;
        }

        void AbortSequence() noexcept override {
            m_encodedBytes = 0u;
        }

        [[nodiscard]] std::uint64_t EncodedBytes() const noexcept {
            return m_encodedBytes;
        }

    private:
        DataCodecExecutionResources m_outputRoot{ResolvedResourceConfiguration{
            {8u * 1024u * 1024u, 1u, 1u}, 8u * 1024u * 1024u, 1u, false, true}};
        std::uint64_t m_encodedBytes{0u};
    };

    TemporalSequenceSource source(keyFrameDataset, predictedDataset);
    TemporalSequenceOutput output;
    const auto encoded = FrameSequenceEncodeExecutor::Execute(FrameSequenceEncodeRequest{
        .source = &source,
        .outputSink = &output,
        .controlParams = &params,
        .resources = {.mode = CodecResourceMode::Fixed, .maxComputeThreads = 1u},
    });
    if (!encoded.success || encoded.encodedFrameCount != 2u || output.EncodedBytes() == 0u) {
        if (encoded.failure) {
            return validation::AssignError(error, FormatCodecFailure(*encoded.failure));
        }
        return validation::AssignError(
            error,
            "temporal pipeline did not produce encoded frame outputs");
    }
    const auto hasPredictorReference = std::any_of(
        encoded.messages.begin(),
        encoded.messages.end(),
        [](const TelemetryMessageRecord& message) {
            return message.origin == "ReferenceEncode.PredictorSpatialBlock" &&
                message.text == "stage result=Completed";
        });
    const auto hasPackageZstd = std::any_of(
        encoded.messages.begin(),
        encoded.messages.end(),
        [](const TelemetryMessageRecord& message) {
            return message.origin == "PackageFieldZstd.Streaming" &&
                message.text == "stage result=Completed";
        });
    const auto topologyStageCount = static_cast<std::size_t>(std::count_if(
        encoded.messages.begin(),
        encoded.messages.end(),
        [](const TelemetryMessageRecord& message) {
            return message.origin == "TopoStage" &&
                message.text == "stage result=Completed";
        }));

    TemporalBuilder::TemporalHistoryState temporalHistory;
    TemporalFrame keyFramePlan;
    TemporalFrame predictedPlan;
    std::string temporalError;
    if (!TemporalBuilder::BuildFrame(
            keyFrameAdapter,
            2u,
            0u,
            params.attrReference,
            params.geometryReference,
            params.topologyReference,
            temporalHistory,
            keyFramePlan,
            &temporalError) ||
        !TemporalBuilder::BuildFrame(
            predictedAdapter,
            2u,
            1u,
            params.attrReference,
            params.geometryReference,
            params.topologyReference,
            temporalHistory,
            predictedPlan,
            &temporalError)) {
        return validation::AssignError(error, temporalError);
    }
    const auto topologyReused =
        predictedPlan.topologyLeaves.size() == 1u &&
        predictedPlan.topologyLeaves[0].ownershipMode == TopologyOwnershipMode::Reused &&
        predictedPlan.topologyLeaves[0].ownerFrameIndex == 0u;
    return Require(
        result,
        hasPredictorReference && hasPackageZstd && topologyReused && topologyStageCount == 1u,
        "pipeline.temporalExecution",
        "temporal pipeline did not omit the reused topology stage or complete reference and package encoding");
}

inline bool CheckEncodeSessionContracts(
    TestResult& result,
    std::string* error = nullptr) {
    auto dataset = MakePipelineContractUnstructuredDataset();
    TestBlockTreeAdapter adapter(dataset);
    auto params = CodecControlParamsFactory::MakeDefault();
    params.attrReference.temporalField.forcePredFrames = true;
    params.attrReference.temporalField.keyFrameInterval = 0u;
    params.geometryReference.temporalField.forcePredFrames = true;
    params.geometryReference.temporalField.keyFrameInterval = 0u;

    EncodeSessionWorkspace workspace;
    DataCodecExecutionResources root(CodecResourceParams{});
    FrameEncodeState firstPlan;
    if (!workspace.PrepareFrame(
            DataCodecEncodeFrameInput{
                .blockTreeAdapter = &adapter,
                .rootName = dataset.name,
                .frameIndex = 0u,
                .frameCount = 2u,
                .controlParams = &params,
            },
            firstPlan,
            root,
            error)) {
        return false;
    }
    const bool callerParamsPreserved =
        firstPlan.controlParams == &params && !firstPlan.defaultControlParams &&
        firstPlan.controlParams->attrReference.temporalField.forcePredFrames &&
        firstPlan.controlParams->attrReference.temporalField.keyFrameInterval == 0u &&
        firstPlan.controlParams->geometryReference.temporalField.forcePredFrames &&
        firstPlan.controlParams->geometryReference.temporalField.keyFrameInterval == 0u;

    FrameEncodeState repeatedPlan;
    std::string repeatedError;
    const bool rejectedRepeatedFrame = !workspace.PrepareFrame(
        DataCodecEncodeFrameInput{
            .blockTreeAdapter = &adapter,
            .rootName = dataset.name,
            .frameIndex = 0u,
            .frameCount = 2u,
            .controlParams = &params,
        },
        repeatedPlan,
        root,
        &repeatedError);

    workspace.ResetSession();
    FrameEncodeState resetPlan;
    std::string resetError;
    const bool resetAccepted = workspace.PrepareFrame(
        DataCodecEncodeFrameInput{
            .blockTreeAdapter = &adapter,
            .rootName = dataset.name,
            .frameIndex = 0u,
            .frameCount = 2u,
            .controlParams = &params,
        },
        resetPlan,
        root,
        &resetError);

    workspace.ResetSession();
    FrameEncodeState treeSignatureFirstPlan;
    std::string treeSignatureFirstError;
    const bool treeSignatureFirstAccepted = workspace.PrepareFrame(
        DataCodecEncodeFrameInput{
            .blockTreeAdapter = &adapter,
            .rootName = dataset.name,
            .frameIndex = 0u,
            .frameCount = 2u,
            .controlParams = &params,
        },
        treeSignatureFirstPlan,
        root,
        &treeSignatureFirstError);
    TestBlockTreeAdapter differentTreeAdapter(dataset, "different-leaf");
    FrameEncodeState differentTreePlan;
    std::string differentTreeError;
    const bool rejectedDifferentTree = !workspace.PrepareFrame(
        DataCodecEncodeFrameInput{
            .blockTreeAdapter = &differentTreeAdapter,
            .rootName = dataset.name,
            .frameIndex = 1u,
            .frameCount = 2u,
            .controlParams = &params,
        },
        differentTreePlan,
        root,
        &differentTreeError);

    return Require(
        result,
        callerParamsPreserved && rejectedRepeatedFrame && !repeatedError.empty() && resetAccepted &&
            treeSignatureFirstAccepted && treeSignatureFirstError.empty() &&
            rejectedDifferentTree && !differentTreeError.empty(),
        "pipeline.encodeSessionContracts",
        resetError.empty()
            ? "encode session accepted an inconsistent multi-frame block tree or a non-sequential frame"
            : resetError);
}

inline bool CheckEncodeReferenceRetirement(TestResult& result, std::string* error) {
    auto dataset = MakePipelineContractUnstructuredDataset();
    TestBlockTreeAdapter adapter(dataset);
    auto params = CodecControlParamsFactory::MakeDefault();
    params.geometryReference.enabled = true;
    params.geometryReference.temporalField.codec = TemporalFieldReferenceCodec::Predictor;
    params.geometryReference.temporalField.keyFrameInterval = 2u;
    params.geometryReference.temporalField.forcePredFrames = false;
    params.attrReference.enabled = false;
    DataCodecExecutionResources root(ResolvedResourceConfiguration{
        {1024u, 1u, 1u}, 1024u, 1u, false, true, false});
    CodecRunScope scope(root);
    EncodeSessionWorkspace workspace;
    FrameEncodeState keyFrame;
    if (!workspace.PrepareFrame(DataCodecEncodeFrameInput{
            .blockTreeAdapter = &adapter, .frameIndex = 0u, .frameCount = 3u,
            .controlParams = &params,
            .nextFrameReferences = FrameReferenceNeeds{
                .geometry = {TemporalFieldRole::PredFrame, 0u}}}, keyFrame, root, error)) { return false; }
    LeafEncodeRun keyLeaf;
    if (!workspace.PrepareLeaf(keyFrame, 0u, keyLeaf, root, error)) { return false; }
    auto* geometry = keyLeaf.context->currentGeometryReferenceCache;
    if (!geometry) { return false; }
    GeometryStorageParams meta;
    meta.elementCount = 2u;
    meta.dimension = 3;
    meta.dataType = DataType::Float32;
    const std::array<float, 6u> values{1.f, 2.f, 3.f, 4.f, 5.f, 6.f};
    {
        auto phase = WaitForHeavyPhase(root);
        if (!phase || !geometry->BeginGeometry(meta, *keyLeaf.context->referenceByteStoreSession, error) ||
            !geometry->WriteRange(0u, 2u, values.data(), sizeof(values), error) ||
            !geometry->EndGeometry(error)) { return false; }
    }
    workspace.CompleteLeafReferences(keyFrame, keyLeaf);
    FrameEncodeState predictedFrame;
    if (!workspace.PrepareFrame(DataCodecEncodeFrameInput{
            .blockTreeAdapter = &adapter, .frameIndex = 1u, .frameCount = 3u,
            .controlParams = &params,
            .nextFrameReferences = FrameReferenceNeeds{
                .geometry = {TemporalFieldRole::KeyFrame, 2u}}}, predictedFrame, root, error)) { return false; }
    LeafEncodeRun predictedLeaf;
    if (!workspace.PrepareLeaf(predictedFrame, 0u, predictedLeaf, root, error)) { return false; }
    const auto& active = predictedLeaf.context->geometryKeyFrameReference.geometryReferenceCache;
    std::weak_ptr<DecodedGeometryReferenceCache> previous = active;
    std::array<float, 6u> decoded{};
    const bool survived = active && active->ReadRange(0u, 2u, decoded.data(), sizeof(decoded), error) && decoded == values;
    workspace.CompleteLeafReferences(predictedFrame, predictedLeaf);
    Require(result, survived && previous.expired() && root.StorageCapacity()->Snapshot().reservedBytes == 0u,
        "pipeline.reference-last-consumer",
        "reference must survive its consumer and retire before packaging when the next frame starts a new key");

    FrameEncodeState lastFrame;
    if (!workspace.PrepareFrame(DataCodecEncodeFrameInput{
            .blockTreeAdapter = &adapter, .frameIndex = 2u, .frameCount = 3u,
            .controlParams = &params}, lastFrame, root, error)) { return false; }
    Require(result, lastFrame.nextFrameReferences.has_value() &&
        lastFrame.nextFrameReferences->geometry.temporalRole == TemporalFieldRole::SingleFrame,
        "pipeline.reference-final-frame", "final frame must retain no future reference dependency");
    return true;
}

inline numericarray::NumericArraySource MakePipelineContractNumericSource(
    const std::vector<float>& values) {
    return numericarray::NumericArraySource{
        .values = NumericArrayView{
            .scalarType = ScalarType::Float32,
            .layout = ArrayLayout::CompactAOS,
            .origin = ViewBufferOrigin::Borrowed,
            .data = values.data(),
            .tupleCount = values.size(),
            .componentCount = 1,
            .tupleStrideBytes = sizeof(float),
        },
        .layout = numericarray::MakeNumericArrayLayout(
            DataType::Float32,
            sizeof(float),
            values.size(),
            1u),
    };
}

struct PipelineContractCountingFloatSource {
    const std::vector<float>* values{nullptr};
    mutable std::size_t tupleReadCount{0u};
};

inline bool ReadPipelineContractCountingFloatTuple(
    const void* userData,
    const std::size_t tupleIndex,
    void* output,
    std::string* error) {
    const auto* source = static_cast<const PipelineContractCountingFloatSource*>(userData);
    if (source == nullptr || source->values == nullptr || output == nullptr ||
        tupleIndex >= source->values->size()) {
        return validation::AssignError(error, "pipeline contract counting source read is invalid");
    }
    std::memcpy(output, source->values->data() + tupleIndex, sizeof(float));
    ++source->tupleReadCount;
    return true;
}

inline numericarray::NumericArraySource MakePipelineContractCountingNumericSource(
    const PipelineContractCountingFloatSource& source) {
    return numericarray::NumericArraySource{
        .values = NumericArrayView{
            .scalarType = ScalarType::Float32,
            .layout = ArrayLayout::GetterOnly,
            .origin = ViewBufferOrigin::Borrowed,
            .tupleCount = source.values != nullptr ? source.values->size() : 0u,
            .componentCount = 1,
            .userData = &source,
            .getTupleBytes = &ReadPipelineContractCountingFloatTuple,
        },
        .layout = numericarray::MakeNumericArrayLayout(
            DataType::Float32,
            sizeof(float),
            source.values != nullptr ? source.values->size() : 0u,
            1u),
    };
}

inline bool CheckSpatialReferenceBlockSelection(
    TestResult& result,
    std::string* error = nullptr) {
    constexpr std::size_t kBlockElementCount = numericarray::kSpatialBlockElementCount;
    constexpr std::size_t kBlockCount = 3u;
    std::vector<float> reference(kBlockElementCount * kBlockCount, 0.0f);
    std::vector<float> current(reference.size(), 0.0f);
    for (std::size_t index = 0u; index < reference.size(); ++index) {
        const auto position = static_cast<float>(index);
        reference[index] = std::sin(position * 0.031f) + 0.25f * std::cos(position * 0.007f);
        const auto blockIndex = index / kBlockElementCount;
        current[index] = blockIndex == 1u
            ? std::sin(position * 0.173f) + 0.6f * std::cos(position * 0.113f)
            : 1.75f * reference[index] + 0.125f;
    }

    NumericArrayStorageParams meta;
    meta.dataType = DataType::Float32;
    meta.elementCount = current.size();
    meta.dimension = 1;

    DataCodecExecutionResources root(CodecResourceParams{});
    CacheResources resources;
    resources.BindRun(root);
    bytestore::ByteStoreSession byteStoreSession;
    byteStoreSession.BindStorage(std::make_shared<resource::ResidentByteBudget>(8u * 1024u * 1024u), true);
    std::shared_ptr<bytestore::IByteSource> transferCache;
    std::vector<NumericArrayBlockLayoutParams> layouts;
    const auto built = numericarrayreference::BuildNumericArrayReferenceTransferCache(
        meta,
        MakeDefaultAttributeValueCompressor(),
        MakePipelineContractNumericSource(current),
        numericarrayreference::NumericArrayReferenceSourceData{
            .candidate = NumericArrayReferenceCandidate{
                .scope = NumericArrayReferenceScope::IntraArray,
                .localParentFieldIndex = 0u,
            },
            .meta = meta,
            .source = MakePipelineContractNumericSource(reference),
        },
        NumericArrayReferenceCodecId::Affine,
        numericarrayreference::NumericArrayReferenceTransferControl{
            .affineBlockRSquared = 0.90,
            .selectionMode = ReferenceSelectionMode::Auto,
        },
        root,
        transferCache,
        byteStoreSession,
        &layouts,
        error,
        "pipeline_contract_reference");
    if (!built) {
        return false;
    }
    const auto referenceBlockCount = static_cast<std::size_t>(std::count_if(
        layouts.begin(),
        layouts.end(),
        [](const NumericArrayBlockLayoutParams& layout) {
            return NumericArrayBlockModeCodecId(layout.mode) ==
                NumericArrayReferenceCodecId::Affine;
        }));
    const auto ordinaryBlockCount = static_cast<std::size_t>(std::count_if(
        layouts.begin(),
        layouts.end(),
        [](const NumericArrayBlockLayoutParams& layout) {
            return NumericArrayBlockModeCodecId(layout.mode) ==
                NumericArrayReferenceCodecId::NonReference;
        }));
    const auto selected = Require(
        result,
        layouts.size() == kBlockCount &&
            referenceBlockCount > 0u &&
            ordinaryBlockCount > 0u,
        "pipeline.spatialReferenceSelection",
        "spatial block auto mode did not preserve mixed ordinary and reference blocks");
    return selected;
}

inline bool CheckSampledIntraParentSelection(
    TestResult& result,
    std::string* error = nullptr) {
    constexpr std::size_t kElementCount = 1024u;
    constexpr std::size_t kSampleCount = 32u;
    std::vector<float> current(kElementCount, 0.0f);
    std::vector<float> parent(kElementCount, 0.0f);
    std::vector<float> unrelated(kElementCount, 0.0f);
    for (std::size_t index = 0u; index < kElementCount; ++index) {
        const auto position = static_cast<float>(index);
        parent[index] = std::sin(position * 0.019f) + 0.1f * std::cos(position * 0.071f);
        current[index] = parent[index] * 1.75f + 0.125f;
        unrelated[index] = std::cos(position * 0.173f) + 0.6f * std::sin(position * 0.113f);
    }
    std::vector<AttrStorageParams> metas(3u);
    for (std::size_t index = 0u; index < metas.size(); ++index) {
        metas[index].name = index == 0u
            ? "current"
            : (index == 1u ? "parent" : "unrelated");
        metas[index].dataType = DataType::Float32;
        metas[index].elementCount = kElementCount;
        metas[index].dimension = 1;
        metas[index].attachmentType = AttrAttachment::Point;
    }
    PipelineContractCountingFloatSource currentSource{.values = &current};
    PipelineContractCountingFloatSource parentSource{.values = &parent};
    PipelineContractCountingFloatSource unrelatedSource{.values = &unrelated};
    const std::vector<numericarray::NumericArraySource> sources{
        MakePipelineContractCountingNumericSource(currentSource),
        MakePipelineContractCountingNumericSource(parentSource),
        MakePipelineContractCountingNumericSource(unrelatedSource),
    };
    const std::vector<std::size_t> metaIndices{0u, 1u, 2u};
    const std::vector<std::uint8_t> referenceAllowed{1u, 0u, 0u};
    ScratchByteBufferPool scratchBytePool;
    for (const auto codec : {
             IntraFieldReferenceCodec::Affine,
             IntraFieldReferenceCodec::Predictor,
             IntraFieldReferenceCodec::Wavelet}) {
        AttrReferenceControlParams dependency;
        dependency.enabled = true;
        dependency.intraField.codec = codec;
        dependency.intraField.selectionMode = ReferenceSelectionMode::Auto;
        dependency.intraField.sampleCount = kSampleCount;
        dependency.intraField.minimumSampleScore = 0.5;
        dependency.intraField.affine.precheckRSquared = 0.90;
        DataCodecExecutionResources root(CodecResourceParams{});
        CodecRunScope scope(root);
        bytestore::ByteStoreSession session;
        session.BindStorage(root.StorageCapacity(), false);
        EncodeAttributeReferenceSchedule schedule;
        if (!BuildAttributeIntraFieldReferenceSchedule(
                metas,
                sources,
                metaIndices,
                referenceAllowed,
                dependency,
                root,
                session,
                schedule,
                error)) {
            return false;
        }
        if (!Require(
                result,
                schedule.initialized &&
                    root.StorageCapacity()->Snapshot().reservedBytes == 0u &&
                    root.StorageCapacity()->Snapshot().peakReservedBytes >=
                        kSampleCount * (sizeof(std::size_t) + metas.size() * sizeof(float)) &&
                    schedule.entries.size() == 3u &&
                    schedule.entries[0].hasIntraParent &&
                    schedule.entries[0].parentMetaIndex == 1u,
                "pipeline.sampledParentSelection",
                "intra-field parent selection did not use the codec sample score")) {
            return false;
        }
    }
    const bool reused = Require(
        result,
        currentSource.tupleReadCount == kSampleCount * 3u &&
            parentSource.tupleReadCount == kSampleCount * 3u &&
            unrelatedSource.tupleReadCount == kSampleCount * 3u,
        "pipeline.reusedReferenceSamples",
        "intra-field parent selection did not reuse one sample read per field");
    if (!reused) { return false; }
    // 在索引、各字段样本及首条实际候选边处分别拒绝容量
    constexpr auto indexBytes = kSampleCount * sizeof(std::size_t);
    constexpr auto fieldBytes = kSampleCount * sizeof(float);
    constexpr std::array<std::size_t, 5u> capacities{
        0u, indexBytes, indexBytes + fieldBytes, indexBytes + 2u * fieldBytes,
        indexBytes + 3u * fieldBytes};
    for (std::size_t denied = 0u; denied < capacities.size(); ++denied) {
        currentSource.tupleReadCount = 0u;
        parentSource.tupleReadCount = 0u;
        unrelatedSource.tupleReadCount = 0u;
        DataCodecExecutionResources root(ResolvedResourceConfiguration{
            .initialLimits = {capacities[denied], 1u, 1u},
            .storageCeilingBytes = capacities[denied], .computeCeiling = 1u, .threaded = false});
        CodecRunScope scope(root);
        bytestore::ByteStoreSession session;
        session.BindStorage(root.StorageCapacity(), false);
        AttrReferenceControlParams dependency;
        dependency.intraField.sampleCount = kSampleCount;
        EncodeAttributeReferenceSchedule schedule;
        std::string rejection;
        const bool built = BuildAttributeIntraFieldReferenceSchedule(metas, sources, metaIndices,
            referenceAllowed, dependency, root, session, schedule, &rejection);
        const auto expectedReads = denied == 4u ? kSampleCount : 0u;
        const bool clean = !built && !schedule.initialized && !rejection.empty() &&
            root.StorageCapacity()->Snapshot().reservedBytes == 0u &&
            currentSource.tupleReadCount == expectedReads && parentSource.tupleReadCount == expectedReads &&
            unrelatedSource.tupleReadCount == expectedReads;
        scope.Finish(false);
        if (!Require(result, clean, "pipeline.sample-capacity-denial-" + std::to_string(denied),
                "sample preparation or edge growth denial must release all owners without replay")) { return false; }
    }
    return true;
}

inline bool CheckForcedReferenceFailure(
    TestResult& result,
    std::string* error = nullptr) {
    constexpr std::size_t kElementCount = 512u;
    std::vector<float> reference(kElementCount, 0.0f);
    std::vector<float> current(kElementCount, 0.0f);
    for (std::size_t index = 0u; index < kElementCount; ++index) {
        const auto position = static_cast<float>(index);
        reference[index] = std::sin(position * 0.013f);
        current[index] = std::cos(position * 0.173f) + 0.3f * std::sin(position * 0.097f);
    }
    NumericArrayStorageParams meta;
    meta.dataType = DataType::Float32;
    meta.elementCount = kElementCount;
    meta.dimension = 1;
    DataCodecExecutionResources root(CodecResourceParams{});
    CodecRunScope scope(root);
    CacheResources resources;
    resources.BindRun(root);
    bytestore::ByteStoreSession byteStoreSession;
    byteStoreSession.BindRun(root);
    std::shared_ptr<bytestore::IByteSource> transferCache;
    std::string forcedError;
    const auto built = numericarrayreference::BuildNumericArrayReferenceTransferCache(
        meta,
        MakeDefaultAttributeValueCompressor(),
        MakePipelineContractNumericSource(current),
        numericarrayreference::NumericArrayReferenceSourceData{
            .candidate = NumericArrayReferenceCandidate{
                .scope = NumericArrayReferenceScope::IntraArray,
                .localParentFieldIndex = 0u,
            },
            .meta = meta,
            .source = MakePipelineContractNumericSource(reference),
        },
        NumericArrayReferenceCodecId::Affine,
        numericarrayreference::NumericArrayReferenceTransferControl{
            .affineBlockRSquared = 0.999999,
            .selectionMode = ReferenceSelectionMode::Forced,
        },
        root,
        transferCache,
        byteStoreSession,
        nullptr,
        &forcedError,
        "pipeline_contract_forced_reference");
    const auto failure = root.FirstFailure();
    return Require(
        result,
        !built && !transferCache && failure && !forcedError.empty() &&
            std::string_view(failure->message.data()).find("forced reference spatial block was rejected") != std::string_view::npos,
        "pipeline.forcedReferenceFailure",
        "forced reference failure was converted into an ordinary block");
}

inline TestResult RunDataCodecFeaturePipelineContracts() noexcept {
    TestResult result;

    {
        auto dataset = MakePipelineContractUnstructuredDataset();
        TestEncodeAdapter adapter(dataset);
        DataCodecExecutionResources resources(ResolvedResourceConfiguration{
            {1024u, 1u, 1u}, 1024u, 1u, false, true});
        CodecRunScope run(resources);
        EncodeContext context(resources);
        context.adapter = &adapter;
        EncodeLeafWorkspace workspace;
        Require(result, !ExecuteAttributeField(context, workspace,
            {.stageName = "PointAttributeStage"}) && context.HasFailure() &&
            context.FirstFailure()->code == CodecErrorCode::MissingInput,
            "pipeline.attribute-requires-controls", "missing resolved controls must enter the failure path");
    }

    std::string error;
    const auto originalOrder = RemapOrderSource::Original();
    const auto computedIdentityOrder = RemapOrderSource::TryComputed(
        MakeIdentityRemapProvider(4u));
    Require(
        result,
        originalOrder.IsOriginal() &&
            originalOrder.Provider() == nullptr &&
            computedIdentityOrder.has_value() &&
            computedIdentityOrder->IsComputed() &&
            computedIdentityOrder->IsComputedIdentity() &&
            computedIdentityOrder->Provider() != nullptr,
        "pipeline.explicitOrderSources",
        "original and computed identity order sources are not distinguishable");

    const EncodePipelineDescriptor rawPipeline{
        .id = EncodePipelineBindingId::UnstructuredOrdinary,
        .pointOrder = EncodePointOrderMode::Original,
        .cellOrder = EncodeCellOrderMode::Original,
        .referenceEncode = false,
        .packageFields = PackageFieldEncodingParams{
            .mode = PackageFieldEncodingMode::Raw,
        },
    };
    Require(
        result,
        ValidateEncodePipelineDescriptor(rawPipeline, &error),
        "pipeline.rawPackageVariant",
        error.empty() ? "raw package pipeline was rejected" : error);

    std::vector<numericarray::SpatialBlockRange> layout;
    error.clear();
    const auto layoutOk = numericarray::BuildSpatialBlockLayout(10u, 4u, layout, &error);
    Require(
        result,
        layoutOk && layout.size() == 3u,
        "pipeline.spatialBlockCount",
        error.empty() ? "spatial block layout count is invalid" : error);
    if (layout.size() == 3u) {
        Require(
            result,
            layout[0].elementOffset == 0u && layout[0].elementCount == 4u &&
                layout[1].elementOffset == 4u && layout[1].elementCount == 4u &&
                layout[2].elementOffset == 8u && layout[2].elementCount == 2u,
            "pipeline.spatialBlockRanges",
            "spatial block layout is not stable and contiguous");
    }

    CodecStorageParams spatialParams;
    spatialParams.meshType = MeshType::PointSet;
    spatialParams.geomParams.codecType = EncodedFieldCodecType::NumericArrayBlocks;
    spatialParams.geomParams.dataType = DataType::Float32;
    constexpr auto fixedBlockCount = numericarray::kSpatialBlockElementCount;
    spatialParams.geomParams.elementCount = 2u * fixedBlockCount + 3u;
    spatialParams.geomParams.dimension = 3;
    std::vector<numericarray::SpatialBlockRange> storageLayout;
    Require(result, numericarray::BuildSpatialBlockLayout(spatialParams.geomParams.elementCount,
        fixedBlockCount, storageLayout, &error), "pipeline.storageLayout", error);
    for (const auto& range : storageLayout) {
        NumericArrayBlockLayoutParams block;
        block.mode = NumericArrayBlockMode::NonReference;
        block.elementOffset = range.elementOffset;
        block.elementCount = range.elementCount;
        block.encodedByteLength = 3u;
        block.bytesCodec = NumericArrayBytesCodec::NumericArrayCodec;
        for (std::uint32_t component = 0u; component < 3u; ++component) {
            block.componentLayouts.push_back(NumericArrayComponentLayoutParams{
                .componentIndex = component,
                .bytesCodec = NumericArrayBytesCodec::NumericArrayCodec,
                .encodedByteLength = 1u,
            });
        }
        spatialParams.geomParams.blockLayouts.push_back(std::move(block));
    }
    bool requires64Bit = false;
    error.clear();
    Require(
        result,
        ValidateCodecStorageParamsContent(spatialParams, requires64Bit, &error),
        "pipeline.sharedSpatialLayoutParams",
        error.empty() ? "shared spatial block params were rejected" : error);
    auto mismatchedSpatialParams = spatialParams;
    mismatchedSpatialParams.geomParams.blockLayouts[1].elementOffset = fixedBlockCount + 1u;
    requires64Bit = false;
    error.clear();
    Require(
        result,
        !ValidateCodecStorageParamsContent(mismatchedSpatialParams, requires64Bit, &error),
        "pipeline.rejectSpatialLayoutMismatch",
        "numeric array block mismatch was accepted outside the shared spatial layout");

    {
        std::vector<std::uint8_t> bytes;
        CodecStorageParams restored;
        Require(result, SerializeCodecStorageParams(spatialParams, bytes, &error) &&
            DeserializeCodecStorageParams(bytes, restored, &error) &&
            restored.spatialBlockParams.pointElementCount == fixedBlockCount &&
            restored.spatialBlockParams.cellElementCount == fixedBlockCount &&
            restored.geomParams.blockLayouts.back().elementOffset == 2u * fixedBlockCount,
            "pipeline.persist-fixed-granularity", "metadata must preserve the fixed block size and tail boundary");

        for (const auto invalidCase : {0u, 1u, 2u}) {
            auto invalid = spatialParams;
            if (invalidCase == 0u) { invalid.spatialBlockParams.pointElementCount = fixedBlockCount + 1u; }
            if (invalidCase == 1u) { invalid.spatialBlockParams.cellElementCount = fixedBlockCount + 1u; }
            if (invalidCase == 2u) {
                invalid.geomParams.blockLayouts.front().componentLayouts.front().bytesCodec =
                    static_cast<NumericArrayBytesCodec>(2u);
            }
            error.clear();
            Require(result, !SerializeCodecStorageParams(invalid, bytes, &error) && bytes.empty() && !error.empty(),
                "pipeline.reject-unsupported-storage-write", "unsupported storage metadata must fail serialization");
            // 直接构造非法输入，核验读取端独立执行格式校验
            std::ostringstream stream(std::ios::binary | std::ios::out);
            {
                cereal::PortableBinaryOutputArchive archive(stream,
                    cereal::PortableBinaryOutputArchive::Options::LittleEndian());
                archive(CodecStorageParamsHeader{}, invalid);
            }
            const auto malformed = stream.str();
            error.clear();
            CodecErrorCode code{};
            Require(result, !DeserializeCodecStorageParams(
                {reinterpret_cast<const std::uint8_t*>(malformed.data()), malformed.size()},
                restored, &error, &code) && code == CodecErrorCode::InvalidInput && !error.empty(),
                "pipeline.reject-unsupported-storage-read", "unsupported storage metadata must fail deserialization");
        }
    }
    {
        auto dataset = MakePipelineContractUnstructuredDataset();
        TestEncodeAdapter adapter(dataset);
        CodecStorageParams created;
        Require(result, CodecStorageParamsFactory::TryFromEncodeAdapter(adapter, {}, created, &error) &&
            created.spatialBlockParams.pointElementCount == 65536u && created.spatialBlockParams.cellElementCount == 65536u,
            "pipeline.fixed-encode-granularity", "the production storage factory must write fixed point and cell block sizes");
    }

    const auto defaults = CodecControlParamsFactory::MakeEncodeConfiguration({});
    const auto enhanced = CodecControlParamsFactory::MakeEncodeConfiguration(
        DataCodecEncodeOptions{.enableCompressionEnhancement = true});
    const auto explicitOptions = CodecControlParamsFactory::MakeEncodeConfiguration(
        DataCodecEncodeOptions{
            .enableCompressionEnhancement = true,
            .packageZstdLevel = 17,
            .temporalKeyFrameInterval = 5u,
        });
    Require(result,
        defaults.pipelineControl.pointOrder == EncodePointOrderMode::Morton &&
        defaults.pipelineControl.cellOrder == EncodeCellOrderMode::Original &&
        defaults.pipelineControl.packageFields.zstdLevel == 3,
        "pipeline.defaultBusinessConfiguration",
        "default configuration must specify the documented algorithm choices");
    Require(result,
        enhanced.pipelineControl.cellOrder == EncodeCellOrderMode::Morton &&
        enhanced.controlParams.attrReference.temporalField.predictor.enableLocalWindowSearch &&
        enhanced.controlParams.attrReference.temporalField.predictor.searchStrategy ==
            TemporalPredictorSearchStrategy::ExhaustiveEstimatedBytes &&
        enhanced.controlParams.geometryReference.temporalField.predictor.enableLocalWindowSearch &&
        enhanced.controlParams.geometryReference.temporalField.predictor.searchStrategy ==
            TemporalPredictorSearchStrategy::ExhaustiveEstimatedBytes,
        "pipeline.compressionEnhancementSemantics",
        "compression enhancement must enable remap and exhaustive predictor search");
    Require(result,
        enhanced.pipelineControl.packageFields.zstdLevel == defaults.pipelineControl.packageFields.zstdLevel &&
        explicitOptions.pipelineControl.packageFields.zstdLevel == 17,
        "pipeline.explicitAlgorithmOptions",
        "explicit compression options must compose with enhancement");

    error.clear();
    if (!CheckSpatialReferenceBlockSelection(result, &error) && !error.empty()) {
        result.AddFailure("pipeline.spatialReferenceEncode", error);
    }
    error.clear();
    if (!CheckSampledIntraParentSelection(result, &error) && !error.empty()) {
        result.AddFailure("pipeline.codecAwareParentSelection", error);
    }
    error.clear();
    if (!CheckForcedReferenceFailure(result, &error) && !error.empty()) {
        result.AddFailure("pipeline.forcedReferenceFailure", error);
    }
    error.clear();
    if (!CheckResolvedFormalPipeline(result, &error) && !error.empty()) {
        result.AddFailure("pipeline.resolvedFormalStages", error);
    }
    CheckTopologyReuseSpatialDependency(result);
    CheckLargePlanningMetadata(result);
    error.clear();
    if (!CheckFormalPipelineExecution(result, &error) && !error.empty()) {
        result.AddFailure("pipeline.formalExecution", error);
    }
    error.clear();
    if (!CheckTemporalPipelineExecution(result, &error) && !error.empty()) {
        result.AddFailure("pipeline.temporalExecution", error);
    }
    error.clear();
    if (!CheckEncodeSessionContracts(result, &error) && !error.empty()) {
        result.AddFailure("pipeline.encodeSessionContracts", error);
    }
    error.clear();
    if (!CheckEncodeReferenceRetirement(result, &error)) {
        result.AddFailure("pipeline.referenceRetirement", error);
    }

    return result;
}

} // namespace datacodec::test

#endif
