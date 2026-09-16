#include "DataCodec/Storage/ByteIO/EncodedInputAccess.h"
#ifndef DATACODEC_TEST_FEATURE_BRIDGEAUDIT_H
#define DATACODEC_TEST_FEATURE_BRIDGEAUDIT_H

#include "DataCodec/Test/Feature/DataCodecFeatureAdapterRoundTrip.h"
#include "DataCodec/Storage/ByteIO/FileByteRangeIO.h"
#include "DataCodec/Storage/ByteIO/FileByteRangeIOTestHooks.h"
#include "DataCodec/Storage/ByteStore/DecodedBufferAccess.h"
#include "DataCodec/Storage/Package/PackageBinaryHeader.h"
#include "DataCodec/Localization/DataCodecMessageCatalog.h"
#include "DataCodec/Runtime/Execution/DataCodecExecutionResources.h"
#include "DataCodec/API/Entry/DataCodecFrameSequenceEncode.h"
#include "DataCodec/Storage/FramePackage/FramePackageIO.h"
#include "DataCodec/Storage/FramePackage/FramePackageSeries.h"
#include "DataCodec/Storage/FramePackage/FrameSequenceFileOutput.h"
#include "DataCodec/API/Entry/InspectEncodedInput.h"
#include "DataCodec/Workflow/Common/PipelineStageBase.h"

#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <chrono>

namespace datacodec::test {

template<class T>
class ExactIntegerGetter final : public IEncodeAttrView {
public:
    std::array<T, 4> values{static_cast<T>((std::uint64_t{1} << 53u) + 1u),
        std::numeric_limits<T>::max(), std::numeric_limits<T>::lowest(), static_cast<T>(7)};
    std::string GetName() const override { return std::is_signed_v<T> ? "signed" : "unsigned"; }
    DataType GetDataType() const override { return std::is_signed_v<T> ? DataType::Int64 : DataType::UInt64; }
    AttrRole GetRole() const override { return AttrRole::Scalar; }
    AttrAttachment GetAttachType() const override { return AttrAttachment::Point; }
    int GetComponentCount() const override { return 1; }
    std::size_t GetElementCount() const override { return values.size(); }
    bool GetTupleBytes(std::size_t index, void* output, std::string* error = nullptr) const override {
        if (index >= values.size() || output == nullptr) { return validation::AssignError(error, "invalid integer tuple"); }
        std::memcpy(output, &values[index], sizeof(T));
        return true;
    }
};

class ExactGeometryInput final : public TestEncodeAdapter {
public:
    explicit ExactGeometryInput(std::shared_ptr<const TestDataset> data)
        : TestEncodeAdapter(*data), datasetOwner(std::move(data)) {}
    std::shared_ptr<const TestDataset> datasetOwner;
    std::array<double, 12> points{std::nextafter(1.0, 2.0), 0.12500000000000003, 0.0,
        16777217.0, 0.0, 0.0, 0.0, 1.0000000000000002, 0.0, 0.0, 0.0, -1.0000000000000002};
    ExactIntegerGetter<std::int64_t> signedValues;
    ExactIntegerGetter<std::uint64_t> unsignedValues;
    ScalarType GetPointScalarType() const override { return ScalarType::Float64; }
    const float* TryGetPointsF32() const override { return nullptr; }
    const double* TryGetPointsF64() const override { return points.data(); }
    void GetPoint(std::size_t index, double output[3]) const override { std::copy_n(points.data() + index * 3u, 3u, output); }
    std::size_t GetNumberOfPointAttrs() const override { return 2u; }
    const IEncodeAttrView& GetPointAttr(std::size_t index) const override {
        return index == 0u ? static_cast<const IEncodeAttrView&>(signedValues) : unsignedValues;
    }
};

class AuditFrameSource final : public IFrameSequenceEncodeSource {
public:
    std::vector<TestDataset> frames{3u, MakeAdapterRoundTripDataset()};
    std::optional<std::size_t> failureOrdinal;
    std::stop_source* cancellation{nullptr};
    std::size_t FrameCount() const noexcept override { return frames.size(); }
    bool LoadFrame(std::size_t ordinal, FrameSequenceEncodeFrame& frame, std::string* error) override {
        if (failureOrdinal == ordinal) { return validation::AssignError(error, "injected sequence source failure"); }
        if (ordinal == 1u && cancellation) { cancellation->request_stop(); }
        frame.blockTreeAdapter = std::make_shared<TestBlockTreeAdapter>(frames.at(ordinal));
        frame.rootName = "audit";
        frame.frameIndex = static_cast<std::uint32_t>(ordinal);
        frame.timeValue = static_cast<float>(ordinal);
        return true;
    }
};

inline void TestSequenceOutputOwnership(TestResult& result, const std::filesystem::path& directory) {
    auto source = std::make_shared<AuditFrameSource>();
    FrameSequenceEncodeRequest request{.source = source,
        .resources = {.mode = CodecResourceMode::Unlimited, .maxComputeThreads = 2u}};
    auto memory = EncodeFrameSequence(request);
    if (!Require(result, memory.success && memory.frames.size() == 3u,
        "bridge.sequence.memory", "core sequence memory output failed")) {
        if (memory.failure) { result.AddDiagnostic(FormatCodecFailure(*memory.failure)); }
        return;
    }
    for (std::size_t i = 0u; i < memory.frames.size(); ++i) {
        MemoryByteRangeReader reader(std::move(memory.frames[i].bytes));
        FramePackage package;
        Require(result, FramePackageIO::ReadMetadata(reader, package) && package.frameIndex == i,
            "bridge.sequence.memory-frame", "transferred frame result has an incorrect identity");
    }
    request.files = FrameSequenceFileTarget{.path = directory / "sequence.igc"};
    const auto initial = EncodeFrameSequence(request);
    if (!Require(result, initial.success && initial.frames.size() == 3u,
        "bridge.sequence.files", "core sequence file output failed")) {
        if (initial.failure) { result.AddDiagnostic(FormatCodecFailure(*initial.failure)); }
        return;
    }
    std::vector<std::vector<std::uint8_t>> originals;
    for (const auto& frame : initial.frames) {
        FileByteRangeReader reader(frame.path);
        originals.emplace_back(static_cast<std::size_t>(reader.ByteSize()));
        Require(result, reader.ReadAt(0u, originals.back()), "bridge.sequence.snapshot", "could not snapshot initial sequence");
    }
    for (const bool cancel : {false, true}) {
        std::stop_source cancellation;
        source->failureOrdinal = cancel ? std::optional<std::size_t>{} : std::optional<std::size_t>{1u};
        source->cancellation = cancel ? &cancellation : nullptr;
        request.stopToken = cancellation.get_token();
        const auto failed = EncodeFrameSequence(request);
        Require(result, !failed.success && failed.frames.empty() && failed.encodedFrameCount == 0u &&
            (!cancel || (failed.failure && failed.failure->cancelled)),
            "bridge.sequence.failure", "failed sequence published a partial result");
        for (std::size_t i = 0u; i < initial.frames.size(); ++i) {
            FileByteRangeReader reader(initial.frames[i].path);
            std::vector<std::uint8_t> actual(static_cast<std::size_t>(reader.ByteSize()));
            Require(result, reader.ReadAt(0u, actual) && actual == originals[i],
                "bridge.sequence.preserve", "failed sequence modified pre-existing output");
        }
    }
    source->failureOrdinal.reset();
    source->cancellation = nullptr;
    request.stopToken = {};
    source->frames.resize(2u);
    const auto replaced = EncodeFrameSequence(request);
    Require(result, replaced.success && replaced.frames.size() == 2u &&
        !std::filesystem::exists(initial.frames.back().path),
        "bridge.sequence.replace", "successful shorter sequence retained obsolete frames");
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        Require(result, !entry.path().filename().string().starts_with(".datacodec-sequence-"),
            "bridge.sequence.cleanup", "sequence retained temporary files after completion");
    }
}

inline TestResult RunDataCodecFeatureBridgeAudit() {
    TestResult result;
    {
        DataCodecExecutionResources run({.mode = CodecResourceMode::Unlimited, .maxComputeThreads = 1u});
        Require(result, run.BeginRun(), "bridge.export-cancel.begin", "cancellation fixture could not start");
        bytestore::ByteStoreSession stores;
        stores.BindRun(run);
        struct CancellingSource final : bytestore::IByteSource {
            DataCodecExecutionResources& run;
            mutable std::size_t reads{0u};
            explicit CancellingSource(DataCodecExecutionResources& value) : run(value) {}
            std::uint64_t ByteSizeHint() const noexcept override { return 2u * kIoWindowBytes; }
            bool CanRead() const noexcept override { return true; }
            bool Read(std::uint64_t, std::span<std::uint8_t> output, std::string*) const override {
                ++reads;
                std::fill(output.begin(), output.end(), 7u);
                run.RequestStop();
                return true;
            }
            bool CopyTo(bytestore::IByteWriter&, std::string*) override { return false; }
        } source(run);
        DecodedBuffer output;
        std::string error;
        Require(result, !DecodedBufferAccess::Export(source, stores, output, &error) &&
            output.empty() && source.reads == 1u && run.StorageCapacity()->Snapshot().reservedBytes == 0u,
            "bridge.export-cancel.release", "cancelled transfer published or retained a partial allocation");
        run.CancelAndWaitRun();
        (void)run.EndRun();
    }
    {
        DecodedBuffer buffer;
        std::weak_ptr<resource::ResidentByteBudget> lifetime;
        const std::array<std::uint8_t, 4u> expected{2u, 5u, 8u, 11u};
        {
            DataCodecExecutionResources run({.mode = CodecResourceMode::Unlimited, .maxComputeThreads = 1u});
            Require(result, run.BeginRun(), "bridge.decoded.begin", "decode ownership fixture could not begin");
            lifetime = run.StorageCapacity();
            bytestore::ByteStoreSession stores;
            stores.BindRun(run);
            auto store = stores.CreateSizedStore(bytestore::ByteStorePurpose::Contiguous,
                expected.size(), MemoryDemandKind::RequiredContinuation, "owned_test");
            Require(result, store && store->WriteBytesAt(0u, expected) && store->Seal(),
                "bridge.decoded.write", "decode ownership fixture could not write");
            const auto* address = store->ContiguousBytes().data();
            Require(result, DecodedBufferAccess::Export(*store, stores, buffer) && buffer.data() == address &&
                run.StorageCapacity()->Snapshot().reservedBytes == expected.size() &&
                run.StorageCapacity()->AllocatedStorage().liveBytes == 0u,
                "bridge.decoded.transfer", "decoded result copied bytes or lost the live cache reservation");
            std::array<std::uint8_t, 4u> actual{};
            Require(result, store->Read(0u, actual) && actual == expected && !store->WriteBytesAt(0u, expected),
                "bridge.decoded.reference", "result transfer invalidated or left writable its cached reference");
            DecodedBuffer shared;
            Require(result, DecodedBufferAccess::Export(*store, stores, shared) && shared.data() == address,
                "bridge.decoded.repeat", "repeated ownership transfer allocated another array");
            store.reset();
            Require(result, run.StorageCapacity()->Snapshot().reservedBytes == 0u,
                "bridge.decoded.retire-cache", "released cache retained its execution reservation");
            Require(result, run.EndRun(), "bridge.decoded.end", "decode ownership fixture could not finish");
        }
        Require(result, lifetime.expired() && std::equal(buffer.span().begin(), buffer.span().end(), expected.begin()),
            "bridge.decoded.lifetime", "decoded result retained its budget or lost its values");
        auto moved = std::move(buffer);
        Require(result, buffer.empty() && buffer.capacity() == 0u && moved.size() == expected.size(),
            "bridge.decoded.move", "moved-from decoded buffer retained a stale range");
    }
    Require(result, MakeStageId(StageKind::Geometry, "Geometry") ==
        MakeStageId(StageKind::Geometry, "几何") &&
        MakeStageId(StageKind::Geometry, "same") != MakeStageId(StageKind::Topology, "same"),
        "bridge.stage.identity", "stage identity depends on display text");
    auto dataset = std::make_shared<const TestDataset>(MakeAdapterRoundTripDataset());
    auto inputOwner = std::make_shared<ExactGeometryInput>(dataset);
    auto& input = *inputOwner;
    const auto expectedPoints = input.points;
    const auto expectedSigned = input.signedValues.values;
    const auto expectedUnsigned = input.unsignedValues.values;
    const std::weak_ptr<const TestDataset> datasetLifetime = dataset;
    const std::weak_ptr<ExactGeometryInput> adapterLifetime = inputOwner;
    EncodeRequest request{
        .input = EncodeInput::LeafAdapter(inputOwner),
        .output = EncodeOutput::Memory(EncodePackageKind::LeafPackage),
        .resources = {.mode = CodecResourceMode::Unlimited, .maxComputeThreads = 2u},
    };
    request.configuration.pipelineControl.pointOrder = EncodePointOrderMode::Original;
    inputOwner.reset();
    dataset.reset();
    Require(result, !adapterLifetime.expired() && !datasetLifetime.expired(),
        "bridge.input-retained", "request did not retain its input and getter state");
    auto encoded = Encode(request);
    request.input = {};
    Require(result, adapterLifetime.expired() && datasetLifetime.expired(),
        "bridge.input-released", "completed encode retained shared input state");
    if (!Require(result, encoded.success, "bridge.exact.encode", "typed encoding failed")) {
        if (encoded.failure) { result.AddDiagnostic(FormatCodecFailure(*encoded.failure)); }
        return result;
    }
    auto owner = std::make_shared<const EncodedBuffer>(std::move(encoded.encodedBytes));
    auto reader = std::make_shared<MemoryByteRangeReader>(owner);
    TestDecodeAdapter output;
    auto decoded = DecodePackage({.input = EncodedInput::Memory(owner), .resources = {.mode = CodecResourceMode::Unlimited, .maxComputeThreads = 2u}});
    if (decoded.success && decoded.output.leaves.size() == 1u) { output.Import(decoded.output.leaves.front()); }
    Require(result, decoded.success && output.PointType() == DataType::Float64 &&
        output.DoublePoints().size() == expectedPoints.size() &&
        std::memcmp(output.DoublePoints().data(), expectedPoints.data(), sizeof(expectedPoints)) == 0,
        "bridge.float64", "Float64 geometry was narrowed or changed");
    const auto& fields = output.Attributes();
    Require(result, fields.size() == 2u, "bridge.integer.fields", "integer attributes are missing");
    if (fields.size() == 2u) {
        Require(result, fields[0].metadata.dataType == DataType::Int64 && fields[0].bytes.size() == sizeof(expectedSigned) &&
            std::memcmp(fields[0].bytes.data(), expectedSigned.data(), sizeof(expectedSigned)) == 0,
            "bridge.int64", "getter-only Int64 lost precision");
        Require(result, fields[1].metadata.dataType == DataType::UInt64 && fields[1].bytes.size() == sizeof(expectedUnsigned) &&
            std::memcmp(fields[1].bytes.data(), expectedUnsigned.data(), sizeof(expectedUnsigned)) == 0,
            "bridge.uint64", "getter-only UInt64 lost precision");
    }

    EncodedBuffer transferred;
    std::weak_ptr<resource::ResidentByteBudget> budgetLifetime;
    const std::array<std::uint8_t, 4u> outputValues{1u, 7u, 13u, 255u};
    {
        DataCodecExecutionResources run(CodecResourceParams{.mode = CodecResourceMode::Unlimited, .maxComputeThreads = 1u});
        Require(result, run.BeginRun(), "bridge.transfer.begin", "ownership fixture failed to start");
        auto budget = run.StorageCapacity();
        budgetLifetime = budget;
        MemoryByteRangeOutput memory(run);
        Require(result, memory.WriteAt(0u, outputValues) && memory.Finalize(outputValues.size()),
            "bridge.transfer.write", "ownership fixture failed to write");
        const auto* allocation = memory.Bytes().data();
        const auto ownedBytes = budget->AllocatedStorage().liveBytes;
        transferred = memory.TakeBytes();
        Require(result, transferred.data() == allocation && transferred.capacity() == ownedBytes &&
            budget->Snapshot().reservedBytes == 0u && budget->AllocatedStorage().liveBytes == 0u &&
            budget->AllocatedStorage().transferredBytes == ownedBytes && memory.Bytes().empty(),
            "bridge.transfer.zero-copy", "encoded result copied bytes or retained execution capacity");
        Require(result, run.EndRun(), "bridge.transfer.end", "ownership fixture failed to finish");
    }
    Require(result, budgetLifetime.expired() && transferred.size() == outputValues.size() &&
        std::equal(transferred.span().begin(), transferred.span().end(), outputValues.begin()),
        "bridge.transfer.lifetime", "result retained the execution budget or lost its bytes after completion");
    auto moved = std::move(transferred);
    Require(result, transferred.empty() && transferred.capacity() == 0u && moved.size() == outputValues.size(),
        "bridge.transfer.move", "moved-from encoded buffer retained a stale range");

    for (const auto code : {CodecErrorCode::IncompleteInput, CodecErrorCode::UnsupportedVersion, CodecErrorCode::InvalidFormat}) {
        auto bytes = std::make_shared<std::vector<std::uint8_t>>(owner->span().begin(), owner->span().end());
        if (code == CodecErrorCode::IncompleteInput) { bytes->resize(2u); }
        else if (code == CodecErrorCode::UnsupportedVersion) { (*bytes)[4] ^= 0x7fu; }
        else { (*bytes)[0] ^= 0x7fu; }
        auto failed = DecodePackage({.input = ::datacodec::EncodedInputAccess::Retain(std::make_shared<MemoryByteRangeReader>(bytes))});
        Require(result, !failed.success && failed.failure && failed.failure->code == code,
            "bridge.error.code", "precise input failure was replaced by a generic decode failure");
        if (failed.failure) {
            auto failure = *failed.failure;
            const auto en = FormatCodecFailureMessage(DataCodecLanguage::English, failure);
            const auto zh = FormatCodecFailureMessage(DataCodecLanguage::SimplifiedChinese, failure);
            failure.message.fill('x');
            Require(result, en != zh && FormatCodecFailureMessage(DataCodecLanguage::English, failure) == en,
                "bridge.error.language", "failure presentation depends on diagnostic text");
        }
    }

    const auto directory = std::filesystem::temp_directory_path() /
        ("datacodec-bridge-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(directory);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    } cleanup{directory};
    const auto path = directory / "mapped.igc";
    std::vector<std::uint8_t> bytes(4u * 1024u * 1024u + 137u);
    for (std::size_t i = 0; i < bytes.size(); ++i) { bytes[i] = static_cast<std::uint8_t>(i * 17u); }
    std::string error;
    {
        FileByteRangeOutput file(path);
        Require(result, file.WriteAt(3u, bytes, &error) && file.WriteAt(0u, std::span(bytes).first(3u), &error) &&
            file.Finalize(bytes.size() + 3u, &error), "bridge.file.write", "mapped file write/backfill failed: " + error);
        Require(result, !file.WriteAt(0u, {}, &error), "bridge.file.closed", "finalized output remained writable");
    }
    {
        FileByteRangeReader file(path);
        std::vector<std::uint8_t> actual(bytes.size());
        Require(result, file.ByteSize() == bytes.size() + 3u && file.ReadAt(3u, actual, &error) && actual == bytes,
            "bridge.file.read", "unaligned or window-spanning mapped read changed bytes");
        Require(result, !file.ReadAt(file.ByteSize(), std::span(actual).first(1u), &error),
            "bridge.file.bounds", "out-of-range read succeeded");
    }
    {
        FileByteRangeOutput abandoned(path);
        Require(result, abandoned.WriteAt(0u, std::span(bytes).first(11u), &error), "bridge.file.abandon", error);
    }
    Require(result, std::filesystem::file_size(path) == bytes.size() + 3u &&
        std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator{}) == 1,
        "bridge.file.cleanup", "abandoned output changed target or retained temporary files");
    for (const auto point : {fileiotest::FailurePoint::Resize, fileiotest::FailurePoint::Map,
             fileiotest::FailurePoint::FlushMappedRange, fileiotest::FailurePoint::FlushFile}) {
        {
            FileByteRangeOutput failed(path);
            const auto successfulCalls = point == fileiotest::FailurePoint::Map ||
                point == fileiotest::FailurePoint::FlushMappedRange ? 1u : 0u;
            fileiotest::ScopedFailure inject(point, successfulCalls);
            const bool written = failed.WriteAt(0u, bytes, &error);
            Require(result, point == fileiotest::FailurePoint::FlushFile ? written : !written,
                "bridge.file.inject-write", "file write did not follow the injected failure");
            Require(result, !failed.Finalize(bytes.size(), &error),
                "bridge.file.inject-finalize", "failed file operation published a result");
        }
        FileByteRangeReader preserved(path);
        std::vector<std::uint8_t> actual(bytes.size());
        Require(result, preserved.ByteSize() == bytes.size() + 3u &&
            preserved.ReadAt(3u, actual, &error) && actual == bytes &&
            std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator{}) == 1,
            "bridge.file.inject-cleanup", "file failure changed the target or retained temporary storage");
    }
    {
        FileByteRangeOutput shortened(path);
        Require(result, shortened.WriteAt(0u, bytes, &error) && shortened.Finalize(19u, &error),
            "bridge.file.truncate", "final length was not applied");
    }
    Require(result, std::filesystem::file_size(path) == 19u, "bridge.file.length", "final file has the wrong length");
    {
        FileByteRangeOutput extended(path);
        Require(result, extended.WriteAt(7u, std::span(bytes).first(3u), &error) && extended.Finalize(17u, &error),
            "bridge.file.extend", "file output could not initialize sparse ranges");
        FileByteRangeReader file(path);
        std::array<std::uint8_t, 17u> actual{};
        std::array<std::uint8_t, 17u> expected{};
        std::copy_n(bytes.data(), 3u, expected.data() + 7u);
        Require(result, file.ReadAt(0u, actual, &error) && actual == expected,
            "bridge.file.zero-gaps", "sparse file ranges contained uninitialized bytes");
    }
    std::filesystem::rename(path, directory / "closed.igc");
    TestSequenceOutputOwnership(result, directory);
    {
        const auto encodedPath = directory / "input.igc";
        FileByteRangeOutput file(encodedPath);
        Require(result, file.WriteAt(0u, owner->span(), &error) && file.Finalize(owner->size(), &error),
            "bridge.input.file.write", error);
        const auto info = InspectEncodedInput(EncodedInput::File(encodedPath));
        TestDecodeAdapter decodedFile;
        const auto read = DecodePackage({.input = EncodedInput::File(encodedPath), .resources = {.mode = CodecResourceMode::Unlimited}});
        if (read.success && read.output.leaves.size() == 1u) { decodedFile.Import(read.output.leaves.front()); }
        Require(result, info.success && info.kind == EncodedPackageKind::Leaf && read.success &&
            decodedFile.DoublePoints() == output.DoublePoints(), "bridge.input.file", "path input round trip failed");
        std::filesystem::rename(encodedPath, directory / "input-closed.igc");
        auto bytesOwner = std::make_shared<const std::vector<std::uint8_t>>(owner->span().begin(), owner->span().end());
        std::weak_ptr<const void> lifetime = bytesOwner;
        auto sharedInput = EncodedInput::Memory(bytesOwner);
        const auto* address = bytesOwner->data();
        bytesOwner.reset();
        auto borrowed = EncodedInputAccess::Open(sharedInput);
        Require(result, borrowed->ContiguousRange(0u, borrowed->ByteSize()).data() == address,
            "bridge.input.borrow", "shared input copied its encoded buffer");
        borrowed.reset();
        TestDecodeAdapter retainedOutput;
        const auto memoryRead = DecodePackage({.input = sharedInput, .resources = {.mode = CodecResourceMode::Unlimited}});
        if (memoryRead.success && memoryRead.output.leaves.size() == 1u) { retainedOutput.Import(memoryRead.output.leaves.front()); }
        sharedInput = {};
        Require(result, memoryRead.success && lifetime.expired() && !retainedOutput.DoublePoints().empty(),
            "bridge.input.release", "completed decode retained its encoded input");
        Require(result, !InspectEncodedInput({}).success,
            "bridge.input.missing", "missing input was accepted");
    }
    {
        const auto oldPath = directory / "rollback_0000.igc";
        FileByteRangeOutput previous(oldPath);
        Require(result, previous.WriteAt(0u, outputValues, &error) && previous.Finalize(outputValues.size(), &error),
            "bridge.sequence.rollback.fixture", error);
        FrameSequenceFileOutput sequence({directory / "rollback.igc", 4});
        std::filesystem::path firstPath, secondPath;
        auto first = sequence.OpenFrame(0u, firstPath, &error);
        auto second = sequence.OpenFrame(1u, secondPath, &error);
        Require(result, first && second && first->WriteAt(0u, bytes, &error) && first->Finalize(bytes.size(), &error),
            "bridge.sequence.rollback.write", error);
        first.reset();
        second.reset();
        // 第二帧没有完成，提交已发布首帧后必须恢复原有序列
        Require(result, !sequence.Commit(&error), "bridge.sequence.rollback.failure", "incomplete sequence was published");
        FileByteRangeReader restored(oldPath);
        std::array<std::uint8_t, 4u> actual{};
        Require(result, restored.ByteSize() == outputValues.size() && restored.ReadAt(0u, actual, &error) &&
            actual == outputValues && !std::filesystem::exists(secondPath),
            "bridge.sequence.rollback.restored", "publish failure did not restore previous files");
    }
    return result;
}

} // 命名空间 datacodec::test
#endif
