#include "DataCodec/Storage/ByteIO/EncodedInputAccess.h"
#include "DataCodec/API/Entry/DecodeStorageAnalysis.h"
#include "DataCodec/API/Entry/DataCodecEncodeEntry.h"
#include "DataCodec/Workflow/Session/CodecRunEntry.h"
#include "DataCodec/Workflow/Session/DecodeSession.h"
#include "DataCodec/Storage/FramePackage/FramePackageIO.h"
#include "DataCodec/Storage/LeafPackage/LeafPackageFieldDecodeStream.h"
#include "DataCodec/Test/Adapter/DataCodecTestAdapter.h"
#include "DataCodec/Filter/Adapter/iGameDecodeAdapter.h"
#include "DataCodec/Filter/Adapter/iGameFramePackageDecodeAssembly.h"
#include "DataCodec/Filter/Adapter/iGameEncodeAdapter.h"
#include "DataCodec/Filter/Test/Data/iGameDataCodecDataGenerator.h"
#include "DataCodec/Test/Common/ReferenceCodecTestHarness.h"
#include "DataCodec/Test/Common/DataCodecAllocationFailure.h"
#include "DataCodec/Runtime/Execution/DataCodecResourceController.h"
#include "Codec/iGameWasmDataCodecBridge.h"
#include <zstd.h>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace datacodec::test {
namespace {

void Ensure(bool condition, const std::string& message) {
    if (!condition) { throw std::runtime_error(message); }
}

class BytesWriter final : public bytestore::IByteWriter {
public:
    bool Write(std::span<const std::uint8_t> data, std::string*) override {
        bytes.insert(bytes.end(), data.begin(), data.end());
        return true;
    }
    std::vector<std::uint8_t> bytes;
};

std::shared_ptr<IByteRangeReader> Reader(BytesWriter writer) {
    return std::make_shared<MemoryByteRangeReader>(std::make_shared<const std::vector<std::uint8_t>>(std::move(writer.bytes)));
}

std::shared_ptr<IByteRangeReader> LeafReader(const LeafPackage& leaf) {
    BytesWriter writer;
    std::string error;
    Ensure(LeafPackageIO::WriteLeafPackage(leaf, writer, &error), error);
    return Reader(std::move(writer));
}

LeafPackage EncodeFixture(const TestDataset& dataset, bool outerCompressed, bool independentAttributes = false) {
    auto adapterOwner = std::make_shared<TestEncodeAdapter>(dataset);
    auto& adapter = *adapterOwner;
    auto configuration = MakeDefaultEncodeConfigurationParams();
    if (independentAttributes) {
        configuration.controlParams.attrReference.intraField.codec = IntraFieldReferenceCodec::Disabled;
    }
    auto encoded = Encode(EncodeRequest{.input = EncodeInput::LeafAdapter(adapterOwner),
        .output = EncodeOutput::Memory(EncodePackageKind::LeafPackage),
        .attributeSelection = AttributeSelectionMode::AllAvailable, .configuration = configuration});
    Ensure(encoded.success, encoded.failure ? FormatCodecFailure(*encoded.failure) : "encode failed");
    auto reader = std::make_shared<MemoryByteRangeReader>(std::make_shared<const EncodedBuffer>(std::move(encoded.encodedBytes)));
    LeafPackage leaf;
    std::string error;
    Ensure(LeafPackageIO::ReadFromByteRange(reader, 0u, reader->ByteSize(), leaf, &error), error);
    DataCodecExecutionResources root(ResolvedResourceConfiguration{{64u << 20u, 1u, 1u}, 64u << 20u, 1u, false});
    CodecRunScope scope(root);
    CacheResources runtime;
    runtime.BindRun(root);
    bytestore::ByteStoreSession stores;
    stores.BindRun(root);
    for (auto& field : leaf.fields) {
        std::shared_ptr<bytestore::IByteSource> payload;
        Ensure(decodefield::PrepareLeafPackageFieldPayload(field, runtime, stores, payload, &error), error);
        std::vector<std::uint8_t> raw(field.rawSize);
        Ensure(payload->Read(0u, raw, &error), error);
        if (outerCompressed) {
            std::vector<std::uint8_t> compressed(ZSTD_compressBound(raw.size()));
            const auto size = ZSTD_compress(compressed.data(), compressed.size(), raw.data(), raw.size(), 1);
            Ensure(!ZSTD_isError(size), "fixture outer compression failed");
            compressed.resize(size);
            field.source = std::make_shared<bytestore::VectorByteSource>(std::move(compressed));
            field.compressionType = EncodedFieldCompressionType::ZSTD;
        } else {
            field.source = std::make_shared<bytestore::VectorByteSource>(std::move(raw));
            field.compressionType = EncodedFieldCompressionType::None;
        }
    }
    return leaf;
}

std::shared_ptr<IByteRangeReader> FrameReader(std::vector<LeafPackage> leaves, FramePackage metadata = {}) {
    std::vector<FramePackageIO::LeafPackageWriter> writers;
    for (std::size_t i = 0u; i < leaves.size(); ++i) {
        auto& leaf = leaves[i];
        leaf.path = "leaf" + std::to_string(i);
        metadata.leaves.push_back({.path = leaf.path, .name = leaf.path});
        FramePackageIO::LeafPackageWriter writer;
        std::string error;
        Ensure(MakeFrameLeafPackageWriter(std::move(leaf), writer, &error), error);
        writers.push_back(std::move(writer));
    }
    BytesWriter bytes;
    std::string error;
    Ensure(FramePackageIO::WriteToWriter(metadata, writers, bytes, &error), error);
    return Reader(std::move(bytes));
}

class Assembly final : public IFramePackageDecodeAssembly {
public:
    bool BeginFramePackage(const FramePackage&, std::string*) override { return true; }
    bool AddBranch(const FramePackageBranchRecord&, std::string*) override { return true; }
    std::unique_ptr<IDecodeAdapter> CreateLeafAdapter(const FramePackageLeafRecord&, const LeafPackage&, std::string*) override {
        return std::make_unique<TestDecodeAdapter>();
    }
    bool CommitLeaf(const FramePackageLeafRecord&, IDecodeAdapter& adapter, std::string*) override {
        Ensure(static_cast<TestDecodeAdapter&>(adapter).Committed(), "leaf output must be committed");
        ++commits;
        return true;
    }
    bool EndFramePackage(std::string*) override { return true; }
    unsigned commits{};
};


std::uint64_t Run(const DecodeStorageAnalysisRequest& analysis, bool framed, bool reduced = false, bool threaded = false) {
    const auto planned = AnalyzeDecodeStorage(analysis);
    Ensure(planned.success && planned.minimumExecutionLimitBytes.has_value(),
        planned.failure ? FormatCodecFailure(*planned.failure) : "analysis failed");
    const auto floor = *planned.minimumExecutionLimitBytes;
    const auto limit = reduced && floor != 0u ? floor - 1u : floor;
    const std::size_t workers = threaded ? 2u : 1u;
    DataCodecExecutionResources root(ResolvedResourceConfiguration{{limit, workers, threaded ? 3u : 1u}, limit, workers, threaded, true, false});
    CodecRunScope scope(root);
    DecodeSession session;
    DecodePackageRequest request{.input = analysis.input, .attributeSelection = analysis.attributeSelection,
        .attributeTargets = analysis.attributeTargets};
    const auto decoded = DecodePackageInRun(request, root, &session);
    const auto peak = root.StorageCapacity()->Snapshot().peakReservedBytes;
    if (reduced && floor) {
        Ensure(!decoded.success && root.FirstFailure() && root.FirstFailure()->requestedBytes != 0u,
            "smaller budget must fail capacity admission");
    } else {
        Ensure(decoded.success, decoded.failure ? FormatCodecFailure(*decoded.failure) : "decode failed at calculated budget");
        Ensure(peak == floor, "controlled peak differs from plan: " + std::to_string(peak) + "/" + std::to_string(floor));
        if (!framed) {
            Ensure(!decoded.output.leaves.empty() && !decoded.output.leaves.front().geometry.values.empty(), "real output was not produced");
        }
    }
    std::cout << "storage_case framed=" << framed
        << " selection=" << static_cast<int>(analysis.attributeSelection) << " reduced=" << reduced
        << " threaded=" << threaded
        << " planned=" << floor << " observed=" << peak << " success=" << decoded.success << '\n';
    return floor;
}

void Supplement(const LeafPackage& leaf) {
    auto input = LeafReader(leaf);
    const auto full = AnalyzeDecodeStorage({.input = ::datacodec::EncodedInputAccess::Retain(input)});
    Ensure(full.success, "supplement full analysis");
    const auto initialLimit = *full.minimumExecutionLimitBytes;
    DataCodecExecutionResources root(ResolvedResourceConfiguration{{initialLimit, 1u, 1u}, initialLimit, 1u, false});
    CodecRunScope scope(root);
    DecodeSession session;
    TestDecodeAdapter adapter;
    const auto decoded = DecodePackageInRun({.input = ::datacodec::EncodedInputAccess::Retain(input), .attributeSelection = AttributeSelectionMode::Explicit, .attributeTargets = {{0u, {}, 0u}}}, root, &session);
    Ensure(decoded.success, decoded.failure ? FormatCodecFailure(*decoded.failure) : "initial partial decode failed");
    DecodeStorageAnalysisRequest request{.input = ::datacodec::EncodedInputAccess::Retain(input), .attributeSelection = AttributeSelectionMode::AllAvailable};
    const auto estimate = session.AnalyzeAttributeStorage(request, root);
    Ensure(estimate.success, estimate.failure ? FormatCodecFailure(*estimate.failure) : "supplement analysis failed");
    Ensure(root.UpdateLimits({*estimate.minimumExecutionLimitBytes, 1u, 1u}, true,
        ResourceDecisionReason::MechanismCheck), "apply supplement budget");
    const auto extra = session.SupplementLeafAttributes(LeafDecodeRequest{.adapter = &adapter,
        .leafPackage = &leaf, .attributeSelection = AttributeSelectionMode::AllAvailable, .resources = &root});
    Ensure(extra.success, extra.failure ? FormatCodecFailure(*extra.failure) : "supplement decode failed");
    const auto repeat = session.AnalyzeAttributeStorage(request, root);
    Ensure(repeat.success && *repeat.minimumExecutionLimitBytes + root.Scratch().RetainedFixedBytes() ==
        root.StorageCapacity()->Snapshot().reservedBytes,
        "completed fields must not be allocated twice");
    std::cout << "storage_supplement planned=" << *estimate.minimumExecutionLimitBytes
        << " retained=" << *repeat.minimumExecutionLimitBytes << '\n';
}

void Sequence(const LeafPackage& leaf, bool host, const LeafPackage* keyLeaf = nullptr) {
    FramePackage key;
    key.geometryTemporalRole = TemporalFieldRole::KeyFrame;
    key.attributeTemporalRole = TemporalFieldRole::KeyFrame;
    auto reference = FrameReader({keyLeaf ? *keyLeaf : leaf}, key);
    FramePackage target;
    target.frameIndex = 1u;
    target.geometryTemporalRole = TemporalFieldRole::PredFrame;
    target.attributeTemporalRole = TemporalFieldRole::PredFrame;
    auto input = FrameReader({leaf}, target);
    const auto estimate = AnalyzeDecodeStorage({.input = ::datacodec::EncodedInputAccess::Retain(input), .referenceInputs = {EncodedInputAccess::Retain(reference)},});
    Ensure(estimate.success, estimate.failure ? FormatCodecFailure(*estimate.failure) : "sequence analysis failed");
    const auto limit = *estimate.minimumExecutionLimitBytes;
    DataCodecExecutionResources root(ResolvedResourceConfiguration{{limit, 1u, 1u}, limit, 1u, false});
    CodecRunScope scope(root);
    auto identities = std::make_shared<DecodeSession::FrameIdentityMap>();
    identities->emplace(0u, DecodeSourceIdentity{"storage-analysis-key", "1"});
    identities->emplace(1u, DecodeSourceIdentity{"storage-analysis-target", "1"});
    const DecodeReferenceKey keyIdentity{.source = identities->at(0u), .keyFrameIndex = 0u};
    auto required = root.Caches().ReferenceCache()->RequireFrame(keyIdentity);
    for (const auto& source : {reference, input}) {
        DecodeSession session;
        session.ConfigureReferences(root, identities);
        const auto decoded = DecodePackageInRun({.input = ::datacodec::EncodedInputAccess::Retain(source), }, root, &session);
        Ensure(decoded.success, decoded.failure ? FormatCodecFailure(*decoded.failure) : "sequence decode failed");
        if (host) {
            iGame::iGameFramePackageDecodeAssembly assembly;
            Ensure(assembly.Import(decoded.output), "native sequence result import failed");
        }
    }
    const auto measured = root.StorageCapacity()->Snapshot().peakReservedBytes;
    Ensure(measured == limit, "sequence predicted peak differs: " + std::to_string(limit) + "/" + std::to_string(measured));
    const auto missing = AnalyzeDecodeStorage({.input = ::datacodec::EncodedInputAccess::Retain(input)});
    Ensure(!missing.success && missing.failure && !missing.minimumExecutionLimitBytes, "missing reference must not yield a limit");
    std::cout << "storage_sequence host=" << host << " planned=" << limit << " observed=" << measured << '\n';
}

LeafPackage ReferenceFixture(const LeafPackage& original, NumericArrayReferenceKind kind, std::size_t targetIndex) {
    auto leaf = original;
    TestDecodeAdapter adapter;
    const auto decoded = DecodePackage({.input = ::datacodec::EncodedInputAccess::Retain(LeafReader(leaf))});
    if (decoded.success && decoded.output.leaves.size() == 1u) { adapter.Import(decoded.output.leaves.front()); }
    Ensure(decoded.success && !adapter.Attributes().empty(), "reference fixture initial decode failed");
    const auto& values = adapter.Attributes()[0].bytes;
    std::vector<float> reference(values.size() / sizeof(float));
    std::memcpy(reference.data(), values.data(), values.size());
    auto current = reference;
    for (auto& value : current) { value += 1.0f; }
    ScratchByteBufferPool scratch;
    NumericArrayReferenceEncodedBlock block;
    std::vector<std::uint8_t> reconstructed;
    std::string error;
    Ensure(EncodeDecodeReferenceTestBlock(NumericArrayReferenceCodecId::Predictor,
        MakeAbsoluteErrorNumericArrayCompressor(1.0e-3), scratch, current, reference,
        static_cast<std::size_t>(adapter.Attributes()[targetIndex].metadata.dimension),
        kind, block, reconstructed, &error), error);
    BytesWriter encoded;
    Ensure(WriteNumericArrayReferenceEncodedBlock(encoded, block, &error), error);
    CodecStorageParams params;
    for (const auto& field : leaf.fields) {
        if (field.type != FieldType::Params) { continue; }
        std::vector<std::uint8_t> bytes(field.rawSize);
        Ensure(field.source->Read(0u, bytes, &error) && DeserializeCodecStorageParams(bytes, params, &error), error);
    }
    for (auto& field : leaf.fields) {
        if (field.type != FieldType::Attribute) { continue; }
        std::vector<std::uint8_t> old(field.rawSize), replacement;
        Ensure(field.source->Read(0u, old, &error), error);
        std::size_t offset = 0u;
        for (const auto index : params.attrPayloadOrder) {
            const auto count = params.attrParams[index].binaryCount;
            if (index == targetIndex) { replacement.insert(replacement.end(), encoded.bytes.begin(), encoded.bytes.end()); }
            else { replacement.insert(replacement.end(), old.begin() + offset, old.begin() + offset + count); }
            offset += count;
        }
        field.rawSize = replacement.size();
        field.source = std::make_shared<bytestore::VectorByteSource>(std::move(replacement));
    }
    params.attrParams[targetIndex].blockLayouts = {MakeNumericArrayReferenceBlockLayout(block)};
    params.attrParams[targetIndex].binaryCount = encoded.bytes.size();
    RefreshAttributeDecodeScheduleHints(params);
    for (auto& field : leaf.fields) {
        if (field.type != FieldType::Params) { continue; }
        std::vector<std::uint8_t> bytes;
        Ensure(SerializeCodecStorageParams(params, bytes, &error), error);
        field.rawSize = bytes.size();
        field.source = std::make_shared<bytestore::VectorByteSource>(std::move(bytes));
    }
    std::size_t rawPayloadBytes = 0u;
    for (const auto& field : leaf.fields) { rawPayloadBytes += field.rawSize; }
    leaf.rawFieldBytes = leafpackagewire::ComputeRawLeafPackageSize(leaf.fields.size(), rawPayloadBytes);
    return leaf;
}

LeafPackage CompressRawLeaf(LeafPackage leaf) {
    for (auto& field : leaf.fields) {
        std::vector<std::uint8_t> raw(field.rawSize), compressed(ZSTD_compressBound(field.rawSize));
        Ensure(field.compressionType == EncodedFieldCompressionType::None && field.source->Read(0u, raw), "raw fixture expected");
        const auto size = ZSTD_compress(compressed.data(), compressed.size(), raw.data(), raw.size(), 1);
        Ensure(!ZSTD_isError(size), "outer reference compression failed");
        compressed.resize(size);
        field.source = std::make_shared<bytestore::VectorByteSource>(std::move(compressed));
        field.compressionType = EncodedFieldCompressionType::ZSTD;
    }
    return leaf;
}

void LazyTemporal(const LeafPackage& keyLeaf, const LeafPackage& temporalLeaf) {
    FramePackage key;
    key.geometryTemporalRole = TemporalFieldRole::KeyFrame;
    key.attributeTemporalRole = TemporalFieldRole::KeyFrame;
    const auto reference = FrameReader({CompressRawLeaf(keyLeaf)}, key);
    FramePackage target;
    target.frameIndex = 1u;
    target.geometryTemporalRole = TemporalFieldRole::PredFrame;
    target.attributeTemporalRole = TemporalFieldRole::PredFrame;
    const auto input = FrameReader({CompressRawLeaf(temporalLeaf)}, target);
    constexpr std::uint64_t ceiling = 32u << 20u;
    DataCodecExecutionResources root(ResolvedResourceConfiguration{{ceiling, 1u, 1u}, ceiling, 1u, false});
    CodecRunScope scope(root);
    auto identities = std::make_shared<DecodeSession::FrameIdentityMap>();
    identities->emplace(0u, DecodeSourceIdentity{"lazy-key", "1"});
    identities->emplace(1u, DecodeSourceIdentity{"lazy-target", "1"});
    auto required = root.Caches().ReferenceCache()->RequireFrame({identities->at(0u), 0u});
    {
        DecodeSession keySession;
        keySession.ConfigureReferences(root, identities);
        Assembly assembly;
        auto decoded = DecodePackageInRun({.input = ::datacodec::EncodedInputAccess::Retain(reference), .attributeSelection = AttributeSelectionMode::None}, root, &keySession);
        Ensure(decoded.success, decoded.failure ? FormatCodecFailure(*decoded.failure) : "lazy key decode failed");
    }
    DecodeSession session;
    session.ConfigureReferences(root, identities);
    Assembly assembly;
    auto initial = DecodePackageInRun({.input = ::datacodec::EncodedInputAccess::Retain(input), .attributeSelection = AttributeSelectionMode::None}, root, &session);
    Ensure(initial.success, initial.failure ? FormatCodecFailure(*initial.failure) : "lazy target decode failed");
    const auto priorPeak = root.StorageCapacity()->Snapshot().peakReservedBytes;
    DecodeStorageAnalysisRequest request{.input = ::datacodec::EncodedInputAccess::Retain(input), .referenceInputs = {EncodedInputAccess::Retain(reference)}};
    const auto planned = session.AnalyzeAttributeStorage(request, root);
    Ensure(planned.success, planned.failure ? FormatCodecFailure(*planned.failure) : "lazy reference analysis failed");
    std::cout << "storage_lazy_plan limit=" << *planned.minimumExecutionLimitBytes
        << " baseline=" << root.StorageCapacity()->Snapshot().reservedBytes
        << " stage=" << planned.peakStage << " parts=";
    for (const auto bytes : planned.peakBytesByKind) { std::cout << bytes << ','; }
    std::cout << '\n';
    Ensure(root.UpdateLimits({*planned.minimumExecutionLimitBytes, 1u, 1u}, true,
        ResourceDecisionReason::MechanismCheck), "apply lazy reference limit");
    TestDecodeAdapter adapter;
    LeafPackage identity;
    identity.path = "leaf0";
    const auto decoded = session.SupplementLeafAttributes({.adapter = &adapter, .leafPackage = &identity,
        .frameIndex = 1u, .attributeSelection = AttributeSelectionMode::AllAvailable, .resources = &root});
    if (!decoded.success && root.FirstFailure()) {
        const auto failure = *root.FirstFailure();
        std::cout << "storage_lazy_failure requested=" << failure.requestedBytes.value_or(0u)
            << " reserved=" << failure.reservedBytes.value_or(0u)
            << " limit=" << failure.limitBytes.value_or(0u) << '\n';
    }
    Ensure(decoded.success, decoded.failure ? FormatCodecFailure(*decoded.failure) : "lazy reference decode failed");
    const auto observed = root.StorageCapacity()->Snapshot().peakReservedBytes;
    Ensure(observed == std::max(priorPeak, *planned.minimumExecutionLimitBytes), "lazy reference overlap differs from plan");
    std::cout << "storage_lazy_reference planned=" << *planned.minimumExecutionLimitBytes
        << " observed=" << observed << '\n';
}

class MetadataOnlyReader final : public IByteRangeReader {
public:
    std::shared_ptr<IByteRangeReader> source;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> forbidden;
    std::uint64_t bytesRead{};
    std::uint64_t ByteSize() const noexcept override { return source->ByteSize(); }
    bool ReadAt(std::uint64_t offset, std::span<std::uint8_t> bytes, std::string* error) override {
        for (const auto& [start, end] : forbidden) {
            if (offset < end && offset + bytes.size() > start) {
                return validation::AssignError(error, "analysis attempted to read numeric payload");
            }
        }
        bytesRead += bytes.size();
        return source->ReadAt(offset, bytes, error);
    }
};

void MetadataOnly(const LeafPackage& leaf) {
    auto reader = std::make_shared<MetadataOnlyReader>();
    reader->source = LeafReader(leaf);
    std::uint64_t offset = reader->ByteSize() - leaf.EncodedFieldBytes();
    for (const auto& field : leaf.fields) {
        const auto end = offset + field.ByteSizeHint();
        if (field.type != FieldType::Params) { reader->forbidden.emplace_back(offset, end); }
        offset = end;
    }
    const auto result = AnalyzeDecodeStorage({.input = ::datacodec::EncodedInputAccess::Retain(reader)});
    Ensure(result.success, result.failure ? FormatCodecFailure(*result.failure) : "metadata-only analysis failed");
    std::cout << "storage_metadata_only bytes_read=" << reader->bytesRead << " package_bytes=" << reader->ByteSize() << '\n';
}

void MemoryAdmissionMechanisms() {
    {
        resource::ResidentByteBudget budget(128u), other(128u);
        auto whole = budget.TryReserve(128u);
        auto part = whole->Split(48u);
        auto foreign = other.TryReserve(1u);
        Ensure(part && whole->Bytes() == 80u && budget.Snapshot().reservedBytes == 128u, "split preserves total");
        Ensure(!whole->Split(81u) && !whole->Merge(std::move(*foreign)), "invalid split and cross-budget merge rejected");
        Ensure(whole->Merge(std::move(*part)) && whole->Bytes() == 128u && !*part, "merge preserves total");
        auto array = budget.Allocate(*whole);
        Ensure(array != nullptr && budget.AllocatedStorage().liveBytes == 128u, "actual backing is recorded");
        auto owner = array.Transfer();
        whole->Reset(); whole->Reset();
        Ensure(owner && budget.Snapshot().reservedBytes == 0u && budget.AllocatedStorage().liveBytes == 0u &&
            budget.AllocatedStorage().transferredBytes == 128u, "ownership transfer is separate from physical destruction");
    }
    for (const auto limit : {128u, 384u}) {
        DataCodecExecutionResources root(ResolvedResourceConfiguration{{limit, 4u, 2u}, 1024u, 4u, true});
        CodecRunScope run(root);
        DecodeBlockMemoryPlan plan;
        const auto inputRange = plan.Append<std::uint8_t>(DecodeMemoryRegion::Input, 32u);
        const auto outputRange = plan.Append<std::uint8_t>(DecodeMemoryRegion::Output, 32u);
        plan.Append<std::uint8_t>(DecodeMemoryRegion::Temporary, 64u);
        std::size_t next = 0u, committed = 0u;
        std::vector<std::unique_ptr<std::uint8_t[]>> retained;
        const bool success = RunOrderedBlocks<std::size_t, std::size_t>(root,
            [&] { return next < 8u; },
            [&](std::size_t& input, const SlotLease&, DecodeBlockWorkspace& memory) {
                input = next++;
                auto bytes = memory.View<std::uint8_t>(inputRange);
                bytes.assign(32u, static_cast<std::uint8_t>(input));
                return true;
            },
            [&](std::size_t input, std::size_t& output, WorkerContext&, DecodeBlockWorkspace& memory) {
                if (input == 0u) { std::this_thread::sleep_for(std::chrono::milliseconds(15)); }
                auto bytes = memory.View<std::uint8_t>(outputRange);
                bytes.assign(32u, static_cast<std::uint8_t>(input));
                output = input;
                return true;
            },
            [&](std::size_t output, DecodeBlockWorkspace& memory) {
                Ensure(output == committed++, "ordered commit survives delayed head");
                retained.push_back(memory.TransferOutput());
                Ensure(retained.back()[0] == output, "observer owns a valid result");
                return true;
            }, false, nullptr, [&] { return plan; });
        ResourceDebugSnapshot snapshot;
        const auto snapshotDeadline = ResourceClock::now() + std::chrono::seconds(2);
        while (!root.TryCopyResourceDebugSnapshot(snapshot)) {
            Ensure(ResourceClock::now() < snapshotDeadline, "admission snapshot timeout");
            std::this_thread::yield();
        }
        Ensure(success && committed == 8u, root.FirstFailure() ? FormatCodecFailure(*root.FirstFailure()) : "joint admission completes");
        Ensure(snapshot.peakAdmittedBlocks <= 2u && snapshot.peakActiveComputeUnits <= 4u &&
            snapshot.storage.peakReservedBytes <= limit && snapshot.storage.reservedBytes == root.Scratch().RetainedFixedBytes(),
            "joint admission obeys both ceilings and retains only idle workspace");
        if (limit == 128u) {
            Ensure(snapshot.peakAdmittedBlocks == 1u && snapshot.byteWaitDuration != ResourceClock::duration{},
                "minimum memory serializes a four-compute flow through byte admission");
        } else {
            Ensure(snapshot.peakAdmittedBlocks == 2u, "larger memory admits overlapping blocks");
        }
        Ensure(root.StorageCapacity()->AllocatedStorage().liveBytes == root.Scratch().RetainedFixedBytes(),
            "transferred results are outside owned audit and idle workspace remains accounted");
        Ensure(run.Finish(true) && root.StorageCapacity()->Snapshot().reservedBytes == 0u &&
            root.StorageCapacity()->AllocatedStorage().liveBytes == 0u, "joint admission drains cleanly");
        std::cout << "admission limit=" << limit << " peak_slots=" << snapshot.peakAdmittedBlocks
            << " peak_compute=" << snapshot.peakActiveComputeUnits << '\n';
    }
    {
        DataCodecExecutionResources root(ResolvedResourceConfiguration{{128u, 4u, 2u}, 512u, 4u, true});
        CodecRunScope run(root);
        Ensure(root.BeginFlow(), "begin dynamic flow");
        resource::ResidentByteBudget::Lease first, denied;
        auto slot = root.TryAcquireSlot(128u, first);
        Ensure(slot && root.UpdateLimits({64u, 1u, 2u}, true, ResourceDecisionReason::MechanismCheck), "shrink live limits");
        const auto before = root.EventEpoch();
        Ensure(!root.TryAcquireSlot(1u, denied) && first.Bytes() == 128u, "shrink preserves prior promise");
        first.Reset(); slot.reset();
        root.WaitForChange(before);
        Ensure(root.UpdateLimits({256u, 4u, 2u}, true, ResourceDecisionReason::MechanismCheck), "restore live limits");
        slot = root.TryAcquireSlot(256u, first);
        Ensure(slot && first.Bytes() == 256u, "growth wakes admission");
        first.Reset(); slot.reset();
        Ensure(run.Finish(true), "dynamic flow clean shutdown");
    }
    for (unsigned failureKind = 0u; failureKind < 7u; ++failureKind) {
        DataCodecExecutionResources root(ResolvedResourceConfiguration{{64u, 2u, 2u}, 64u, 2u, true});
        CodecRunScope run(root);
        DecodeBlockMemoryPlan plan;
        const auto range = plan.Append<std::uint8_t>(DecodeMemoryRegion::Temporary, failureKind == 0u ? 65u : 64u);
        unsigned next = 0u;
        const bool ok = RunOrderedBlocks<unsigned, unsigned>(root, [&] { return next < 2u; },
            [&](unsigned& input) { input = next++; return failureKind != 1u; },
            [&](unsigned, unsigned&, WorkerContext&, DecodeBlockWorkspace& memory) {
                if (failureKind == 2u) { root.RequestStop(); return false; }
                if (failureKind == 3u) { throw std::runtime_error("intentional decode worker failure"); }
                if (failureKind == 4u) { memory.View<std::uint8_t>(range).resize(65u); }
                return true;
            }, [&](unsigned&) {
                if (failureKind == 6u) { throw std::runtime_error("intentional observer failure"); }
                return failureKind != 5u;
            }, false, nullptr, [&] { return plan; });
        root.Scratch().ClearFixed();
        Ensure(!ok && root.StorageCapacity()->Snapshot().reservedBytes == 0u &&
            root.StorageCapacity()->AllocatedStorage().liveBytes == 0u, "failure and cancellation retire every backing");
        run.Finish(false);
    }
    for (const bool multiply : {false, true}) {
        bool rejected = false;
        try {
            const auto maximum = std::numeric_limits<std::size_t>::max();
            if (multiply) { (void)DecodeBlockMemoryPlan::Multiply(maximum, 2u); }
            else { (void)DecodeBlockMemoryPlan::Add(maximum, 1u); }
        } catch (const std::length_error&) { rejected = true; }
        Ensure(rejected, "overflowing working layouts are rejected");
    }
    {
        DataCodecExecutionResources root(ResolvedResourceConfiguration{{64u, 2u, 2u}, 64u, 2u, true});
        CodecRunScope scope(root);
        Ensure(root.BeginFlow(), "begin cancelled byte wait");
        auto retained = root.StorageCapacity()->TryReserve(64u);
        resource::ResidentByteBudget::Lease denied;
        Ensure(!root.TryAcquireSlot(1u, denied), "exhausted budget waits before input admission");
        const auto epoch = root.EventEpoch();
        std::jthread cancel([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            root.RequestStop();
        });
        root.WaitForChange(epoch);
        cancel.join();
        Ensure(root.Stopped(), "cancellation wakes byte wait");
        retained.reset();
        Ensure(!scope.Finish(false) && root.StorageCapacity()->Snapshot().reservedBytes == 0u,
            "cancelled byte wait drains capacity");
    }
}

void UnlimitedResources() {
    const CodecResourceParams unlimited{.mode = CodecResourceMode::Unlimited, .maxComputeThreads = 2u};
    {
        resource::ResidentByteBudget capacity(std::nullopt);
        auto large = capacity.TryReserve(std::uint64_t{1} << 40u);
        auto extra = capacity.TryReserve(512u);
        Ensure(large && extra && !capacity.Snapshot().limitBytes, "Unlimited reserves independently of a byte ceiling");
        auto part = extra->Split(128u);
        Ensure(part && extra->Merge(std::move(*part)), "Unlimited leases retain split and merge semantics");
        large.reset(); extra.reset();
        auto maximum = capacity.TryReserve(std::numeric_limits<std::uint64_t>::max());
        bool overflow = false;
        try { (void)capacity.TryReserve(1u); } catch (const std::length_error&) { overflow = true; }
        Ensure(overflow && capacity.Snapshot().reservedBytes == std::numeric_limits<std::uint64_t>::max(),
            "Unlimited detects arithmetic overflow without corrupting reservations");
        maximum.reset();
        Ensure(capacity.Snapshot().reservedBytes == 0u, "Unlimited reservations return to zero");
    }
    for (const bool threaded : {false, true}) {
        ResourceSample sample;
        sample.threaded = threaded;
        sample.allowedComputeThreads = 2u;
        sample.hardLimitBytes = 1u;
        sample.pressure = PressureLevel::Critical;
        DataCodecExecutionResources root(ResolveResourceConfiguration(
            {.mode = CodecResourceMode::Unlimited, .threadMode = CodecThreadMode::Fixed}, sample));
        CodecRunScope scope(root);
        DecodeBlockMemoryPlan plan;
        const auto output = plan.Append<std::uint8_t>(DecodeMemoryRegion::Output, 128u);
        plan.Append<std::uint8_t>(DecodeMemoryRegion::Temporary, 64u);
        unsigned next = 0u, committed = 0u;
        std::vector<std::unique_ptr<std::uint8_t[]>> retained;
        const auto success = RunOrderedBlocks<unsigned, unsigned>(root, [&] { return next < 12u; },
            [&](unsigned& input) { input = next++; return true; },
            [&](unsigned input, unsigned& result, WorkerContext&, DecodeBlockWorkspace& memory) {
                if (input == 0u) { std::this_thread::sleep_for(std::chrono::milliseconds(15)); }
                memory.View<std::uint8_t>(output).assign(128u, static_cast<std::uint8_t>(input));
                result = input;
                return true;
            }, [&](unsigned result, DecodeBlockWorkspace& memory) {
                Ensure(result == committed++, "Unlimited commits delayed blocks in order");
                retained.push_back(memory.TransferOutput());
                Ensure(retained.back()[0] == result, "Unlimited transferred output remains valid");
                return true;
            }, false, nullptr, [&] { return plan; });
        ResourceDebugSnapshot snapshot;
        const auto snapshotDeadline = ResourceClock::now() + std::chrono::seconds(2);
        while (!root.TryCopyResourceDebugSnapshot(snapshot)) {
            Ensure(ResourceClock::now() < snapshotDeadline, "Unlimited admission snapshot timeout");
            std::this_thread::yield();
        }
        Ensure(success && committed == 12u && !snapshot.storage.limitBytes && !snapshot.storageCeilingBytes &&
            !snapshot.byteWaiting && !snapshot.automaticObservationApplicable && snapshot.gateOpen &&
            snapshot.peakAdmittedBlocks <= (threaded ? 3u : 1u) &&
            snapshot.peakActiveComputeUnits <= (threaded ? 2u : 1u), "Unlimited retains bounded computation and admissions");
        root.Scratch().ClearFixed();
        Ensure(root.StorageCapacity()->Snapshot().reservedBytes == 0u &&
            root.StorageCapacity()->AllocatedStorage().liveBytes == 0u && scope.Finish(true),
            "Unlimited drains owned memory after transfers");
    }
    {
        DataCodecExecutionResources root(unlimited);
        CodecRunScope scope(root);
        Ensure(root.BeginFlow(), "Unlimited allocation failure flow");
        auto slot = root.TryAcquireSlot();
        auto capacity = root.StorageCapacity();
        auto lease = capacity->TryReserve(128u);
        Ensure(slot && lease, "Unlimited allocation failure prerequisites");
        const bool completed = RunTerminalWork(root, *slot, [&](WorkerContext&) {
            RejectAllocationsScope rejection;
            auto backing = root.Scratch().AllocateFixed(*capacity, std::move(*lease));
            return backing.size == 128u;
        });
        lease.reset(); slot.reset();
        Ensure(!completed && root.FirstFailure() && !scope.Finish(false) &&
            capacity->Snapshot().reservedBytes == 0u && capacity->AllocatedStorage().liveBytes == 0u,
            "Unlimited reports real allocation failure and releases the promise");
    }
    {
        DataCodecExecutionResources root(unlimited);
        CodecRunScope scope(root);
        Ensure(root.BeginFlow(), "Unlimited cancellation flow");
        auto first = root.TryAcquireSlot(), second = root.TryAcquireSlot(), third = root.TryAcquireSlot();
        Ensure(first && second && third && !root.TryAcquireSlot(), "Unlimited still blocks at slot limit");
        const auto epoch = root.EventEpoch();
        std::jthread cancel([&] { root.RequestStop(); });
        root.WaitForChange(epoch);
        cancel.join();
        first.reset(); second.reset(); third.reset();
        Ensure(root.Stopped() && !scope.Finish(false), "Unlimited cancellation wakes the admission driver");
    }
    for (const auto& dataset : {MakeAdapterRoundTripDataset(), MakePipelineContractUnstructuredDataset()}) {
        auto encodeAdapterOwner = std::make_shared<TestEncodeAdapter>(dataset);
        auto& encodeAdapter = *encodeAdapterOwner;
        TestDecodeAdapter baseline;
        for (const auto mode : {CodecResourceMode::Fixed, CodecResourceMode::Unlimited}) {
            const CodecResourceParams resources{.mode = mode, .maxComputeThreads = 2u,
                .ownedStorageLimitBytes = mode == CodecResourceMode::Fixed
                    ? std::optional<std::uint64_t>(256u << 20u) : std::nullopt};
            auto encoded = Encode({.input = EncodeInput::LeafAdapter(encodeAdapterOwner),
                .output = EncodeOutput::Memory(EncodePackageKind::LeafPackage), .resources = resources});
            Ensure(encoded.success, encoded.failure ? FormatCodecFailure(*encoded.failure) : "Unlimited encode failed");
            auto reader = std::make_shared<MemoryByteRangeReader>(
                std::make_shared<const EncodedBuffer>(std::move(encoded.encodedBytes)));
            TestDecodeAdapter actual;
            auto& adapter = mode == CodecResourceMode::Fixed ? baseline : actual;
            const auto decoded = DecodePackage({.input = ::datacodec::EncodedInputAccess::Retain(reader), .resources = resources});
            if (decoded.success && decoded.output.leaves.size() == 1u) { adapter.Import(decoded.output.leaves.front()); }
            Ensure(decoded.success && adapter.Committed(), decoded.failure ? FormatCodecFailure(*decoded.failure) : "Unlimited decode failed");
            if (mode == CodecResourceMode::Unlimited) {
                Ensure(actual.Points() == baseline.Points() && actual.Connectivity() == baseline.Connectivity() &&
                    actual.Offsets() == baseline.Offsets() && actual.Attributes().size() == baseline.Attributes().size(),
                    "Unlimited geometry and topology match fixed-mode round trip");
                for (std::size_t i = 0u; i < actual.Attributes().size(); ++i) {
                    Ensure(actual.Attributes()[i].complete && actual.Attributes()[i].bytes == baseline.Attributes()[i].bytes,
                        "Unlimited decoded attributes match fixed-mode round trip");
                }
                TestDecodeAdapter denied;
                const auto fixedFailure = DecodePackage({.input = ::datacodec::EncodedInputAccess::Retain(reader), .resources = {.mode = CodecResourceMode::Fixed, .maxComputeThreads = 2u, .ownedStorageLimitBytes = 0u}});
                Ensure(!fixedFailure.success && fixedFailure.failure, "same input fails a zero fixed budget");
                auto bytes = std::make_shared<std::vector<std::uint8_t>>(reader->ByteSize());
                std::string error;
                Ensure(reader->ReadAt(0u, *bytes, &error), error);
                const auto bridge = iGame::DecodeiGameWasmDataCodecMemory(bytes, *bytes, false,
                    {}, unlimited);
                Ensure(bridge.success && bridge.output != nullptr, "WASM memory bridge forwards Unlimited: " + bridge.error);
            }
        }
    }
}

void MultiBatchPolyhedronAdmission() {
    auto mesh = iGame::VolumeMesh::New();
    auto points = iGame::Points::New();
    points->AddPoint(0.0f, 0.0f, 0.0f);
    points->AddPoint(1.0f, 0.0f, 0.0f);
    points->AddPoint(0.0f, 1.0f, 0.0f);
    mesh->SetPoints(points);
    auto faces = iGame::CellArray::New();
    const igIndex triangle[]{0, 1, 2};
    for (std::size_t i = 0u; i < 9u; ++i) { faces->AddCellIds(triangle, 3); }
    auto cells = iGame::CellArray::New();
    const igIndex ids[]{0, 1, 2, 3, 4, 5, 6, 7, 8};
    for (std::size_t i = 0u; i < numericarray::kSpatialBlockElementCount; ++i) { cells->AddCellIds(ids, 4); }
    cells->AddCellIds(ids, 9);
    mesh->InitVolumesWithPolyhedron(faces, cells);
    auto adapterOwner = std::make_shared<iGame::iGameEncodeAdapter>(mesh);
    auto& adapter = *adapterOwner;
    auto encoded = Encode({.input = EncodeInput::LeafAdapter(adapterOwner),
        .output = EncodeOutput::Memory(EncodePackageKind::LeafPackage),
        .attributeSelection = AttributeSelectionMode::None});
    Ensure(encoded.success, encoded.failure ? FormatCodecFailure(*encoded.failure) : "polyhedron multi-batch encode");
    auto input = std::make_shared<MemoryByteRangeReader>(std::make_shared<const EncodedBuffer>(std::move(encoded.encodedBytes)));
    const DecodeStorageAnalysisRequest request{.input = ::datacodec::EncodedInputAccess::Retain(input),};
    const auto analysis = AnalyzeDecodeStorage(request);
    Ensure(analysis.success, "owned topology planning must succeed");
    Run(request, false);
    Run(request, false, true);
    Run(request, false, false, true);
}

}

int RunDataCodecStorageAnalysisTests() try {
    std::cout << std::unitbuf;
    UnlimitedResources();
    MemoryAdmissionMechanisms();
    MultiBatchPolyhedronAdmission();
    for (const bool topology : {false, true}) {
        const auto dataset = topology ? MakePipelineContractUnstructuredDataset() : MakeAdapterRoundTripDataset();
        for (const bool compressed : {false, true}) {
            auto leaf = EncodeFixture(dataset, compressed);
            MetadataOnly(leaf);
            auto input = LeafReader(leaf);
            for (const bool host : {false, true}) {
                for (const auto selection : {AttributeSelectionMode::None, AttributeSelectionMode::Explicit,
                                            AttributeSelectionMode::AllAvailable}) {
                    DecodeStorageAnalysisRequest request{.input = ::datacodec::EncodedInputAccess::Retain(input), .attributeSelection = selection,};
                    if (selection == AttributeSelectionMode::Explicit) { request.attributeTargets = {{0u, {}, 1u}}; }
                    Run(request, false);
                    Run(request, false, true);
                }
                Run({.input = ::datacodec::EncodedInputAccess::Retain(FrameReader({leaf, leaf})),}, true);
                Run({.input = ::datacodec::EncodedInputAccess::Retain(input),}, false, false, true);
            }
            Supplement(leaf);
            Sequence(leaf, false);
            Sequence(leaf, true);
            std::stop_source cancelled;
            cancelled.request_stop();
            const auto cancellation = AnalyzeDecodeStorage({.input = ::datacodec::EncodedInputAccess::Retain(input), .stopToken = cancelled.get_token()});
            Ensure(cancellation.cancelled && !cancellation.minimumExecutionLimitBytes, "cancelled analysis must not publish a limit");
        }
    }
    for (const bool polyhedron : {false, true}) {
        auto source = polyhedron ? iGame::datacodec_test::BuildSyntheticPolyhedronSmokeObject() :
            iGame::datacodec_test::BuildSyntheticStructuredSmokeObject();
        // 此处验证拓扑容量，点属性保持确定长度，单元属性由普通拓扑用例覆盖
        source->SetAttributeSet(iGame::datacodec_test::BuildSyntheticAttributeSet("storage", 8u, 0u, 0.0f, 0.0f));
        auto adapterOwner = std::make_shared<iGame::iGameEncodeAdapter>(source);
        auto& adapter = *adapterOwner;
        auto encoded = Encode({.input = EncodeInput::LeafAdapter(adapterOwner),
            .output = EncodeOutput::Memory(EncodePackageKind::LeafPackage)});
        Ensure(encoded.success, encoded.failure ? FormatCodecFailure(*encoded.failure) : "host fixture encode failed");
        auto reader = std::make_shared<MemoryByteRangeReader>(std::make_shared<const EncodedBuffer>(std::move(encoded.encodedBytes)));
        Run({.input = ::datacodec::EncodedInputAccess::Retain(reader),}, false);
        Run({.input = ::datacodec::EncodedInputAccess::Retain(reader),}, false, true);
    }
    const auto missing = AnalyzeDecodeStorage({});
    Ensure(!missing.success && missing.failure && !missing.minimumExecutionLimitBytes, "missing input must fail cleanly");
    const auto keyLeaf = EncodeFixture(MakeAdapterRoundTripDataset(), false);
    const auto temporalLeaf = ReferenceFixture(keyLeaf, NumericArrayReferenceKind::TemporalKeyFrame, 0u);
    Sequence(temporalLeaf, false, &keyLeaf);
    Sequence(temporalLeaf, true, &keyLeaf);
    LazyTemporal(keyLeaf, temporalLeaf);
    const auto intra = ReferenceFixture(EncodeFixture(MakePipelineContractUnstructuredDataset(), false, true),
        NumericArrayReferenceKind::IntraArray, 1u);
    for (const bool host : {false, true}) {
        Run({.input = ::datacodec::EncodedInputAccess::Retain(LeafReader(intra)), .attributeSelection = AttributeSelectionMode::Explicit,
            .attributeTargets = {{0u, {}, 1u}},}, false);
    }
    NumericArrayStorageParams oversized;
    oversized.elementCount = std::numeric_limits<std::uint64_t>::max();
    oversized.dimension = 3;
    std::uint64_t ignored = 0u;
    Ensure(!CalculateDecodedNumericStorageBytes(oversized, ignored), "overflowing storage shape must be rejected");
    const auto badTarget = AnalyzeDecodeStorage({.input = ::datacodec::EncodedInputAccess::Retain(LeafReader(keyLeaf)),
        .attributeSelection = AttributeSelectionMode::Explicit, .attributeTargets = {{0u, {}, 999u}}});
    Ensure(!badTarget.success && badTarget.failure && !badTarget.minimumExecutionLimitBytes,
        "invalid target must not publish a partial limit");
    std::cout << "DataCodec storage analysis tests passed\n";
    return 0;
} catch (const std::exception& exception) {
    std::cerr << "storage analysis test failed: " << exception.what() << '\n';
    return 1;
}

}
