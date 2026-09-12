#ifndef DATACODEC_TEST_FEATURE_DATACODECFEATUREINPUTCANCELLATION_H
#define DATACODEC_TEST_FEATURE_DATACODECFEATUREINPUTCANCELLATION_H

#include "DataCodec/Storage/LeafPackage/LeafPackageFieldDecodeStream.h"
#include "DataCodec/Storage/LeafPackage/LeafPackageIO.h"
#include "DataCodec/Storage/FramePackage/FramePackageIO.h"
#include "DataCodec/Storage/Package/PackageBinaryHeader.h"
#include "DataCodec/Storage/ByteIO/CallbackByteRangeReader.h"
#include "DataCodec/Test/Common/DataCodecTestResult.h"

namespace datacodec::test {

inline TestResult RunDataCodecFeatureInputCancellation() {
    TestResult result;
    std::stop_source stop;
    std::size_t calls = 0u;
    auto metadata = std::make_shared<CallbackByteRangeReader>(4096u,
        [&](std::uint64_t, std::span<std::uint8_t> bytes, std::string*) {
            ++calls;
            std::fill(bytes.begin(), bytes.end(), 0u);
            stop.request_stop();
            return true;
        });
    PackageInspection inspection;
    Require(result, !InspectPackage(*metadata, inspection, nullptr, stop.get_token()) && calls == 1u,
        "input.cancel-header", "header inspection must stop after cancelled source I/O");
    LeafPackage leaf;
    FramePackage frame;
    Require(result, !LeafPackageIO::ReadFromByteRange(metadata, 0u, metadata->ByteSize(), leaf, nullptr, stop.get_token()) &&
        !FramePackageIO::ReadMetadata(*metadata, frame, nullptr, stop.get_token()) && calls == 1u,
        "input.cancel-metadata", "stopped metadata reads must not invoke the shared source");

    DataCodecExecutionResources run(ResolvedResourceConfiguration{{kIoWindowBytes, 1u, 1u}, kIoWindowBytes, 1u, false, true});
    CacheResources runtime;
    runtime.BindRun(run);
    class Source final : public bytestore::IByteSource {
    public:
        explicit Source(DataCodecExecutionResources& run) : root(run) {}
        std::uint64_t ByteSizeHint() const noexcept override { return 2u * kIoWindowBytes + 3u; }
        bool CanRead() const noexcept override { return true; }
        bool Read(std::uint64_t offset, std::span<std::uint8_t> bytes, std::string*) const override {
            ++calls;
            largest = std::max(largest, bytes.size());
            if (offset > ByteSizeHint() || bytes.size() > ByteSizeHint() - offset) { return false; }
            std::fill(bytes.begin(), bytes.end(), 42u);
            if (cancel) { root.RequestStop(); }
            return true;
        }
        bool CopyTo(bytestore::IByteWriter&, std::string*) override { return false; }
        DataCodecExecutionResources& root;
        bool cancel{true};
        mutable std::size_t calls{0u};
        mutable std::size_t largest{0u};
    };
    auto source = std::make_shared<Source>(run);
    run.BeginRun();
    decodefield::FieldDecodeStreamReader reader;
    decodefield::FieldOutputSegment segment;
    bool present = true;
    Require(result, reader.OpenRaw(source, source->ByteSizeHint(), runtime) &&
        !reader.ReadNext(segment, present) && !present && source->calls == 1u,
        "input.cancel-field", "field decoding must not publish bytes after cancellation during I/O");
    reader = {};
    run.CancelAndWaitRun();
    run.EndRun();
    run.BeginRun();
    source->cancel = false;
    source->calls = 0u;
    Require(result, reader.OpenRaw(source, source->ByteSizeHint(), runtime),
        "input.reopen-field", "the same retained input must be usable in a new independent request");
    std::uint64_t consumed = 0u;
    bool success = true;
    for (;;) {
        if (!reader.ReadNext(segment, present)) { success = false; break; }
        if (!present) { break; }
        consumed += segment.bytes.size();
    }
    Require(result, success && consumed == source->ByteSizeHint() && source->calls == 3u &&
        source->largest == kIoWindowBytes && !run.Stopped(),
        "input.request-token-lifetime", "new field readers must use the current request token and fixed windows");
    reader = {};
    run.EndRun();
    runtime.UnbindRun();
    return result;
}

}

#endif
