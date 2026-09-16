#include "DataCodec/API/Entry/InspectEncodedInput.h"
#include "DataCodec/Storage/ByteIO/EncodedInputAccess.h"
#include "DataCodec/Storage/Package/PackageBinaryHeader.h"
#include "DataCodec/Storage/FramePackage/FramePackageIO.h"

namespace datacodec {

EncodedInputInspection InspectEncodedInput(const EncodedInput& input, std::stop_token stop) {
    EncodedInputInspection result;
    const auto fail = [&](CodecErrorCode code, std::string_view detail) {
        result.failure = MakeCodecFailureRecord(code, "input.inspect", "InspectEncodedInput",
            detail, stop.stop_requested());
    };
    try {
        if (stop.stop_requested()) {
            fail(CodecErrorCode::DecodeFailure, "input inspection cancelled");
            return result;
        }
        auto reader = EncodedInputAccess::Open(input);
        if (!reader) {
            fail(CodecErrorCode::InvalidFormat, "input is missing");
            return result;
        }
        PackageInspection inspection;
        std::string error;
        if (!InspectPackage(*reader, inspection, &error, stop)) {
            result.failure = std::move(inspection.failure);
            return result;
        }
        result.byteSize = inspection.byteSize;
        result.version = inspection.version;
        result.contentIdentity = PackageIdentityToHex(inspection.identity);
        result.sourceIdentity = std::move(inspection.sourceIdentity);
        result.kind = inspection.format == PackageBinaryFormat::FramePackage
            ? EncodedPackageKind::Frame : EncodedPackageKind::Leaf;
        if (result.kind == EncodedPackageKind::Frame) {
            FramePackage frame;
            if (!FramePackageIO::ReadMetadata(*reader, frame, &error, stop)) {
                fail(CodecErrorCode::InvalidFormat, error);
                return result;
            }
            result.frameIndex = frame.frameIndex;
            result.timeValue = frame.timeValue;
        }
        result.success = true;
    } catch (const std::exception& error) {
        fail(CodecErrorCode::DecodeFailure, error.what());
    } catch (...) {
        fail(CodecErrorCode::DecodeFailure, "unknown input inspection error");
    }
    return result;
}

EncodedInputInspection InspectEncodedPrefix(std::span<const std::uint8_t> prefix, std::uint64_t totalBytes) {
    class PrefixReader final : public IByteRangeReader {
    public:
        PrefixReader(std::span<const std::uint8_t> bytes, std::uint64_t total) : bytes(bytes), total(total) {}
        std::uint64_t ByteSize() const noexcept override { return total; }
        bool ReadAt(std::uint64_t offset, std::span<std::uint8_t> output, std::string* error) override {
            if (offset > bytes.size() || output.size() > bytes.size() - offset) {
                return validation::AssignError(error, "package inspection prefix is incomplete");
            }
            if (!output.empty()) { std::memcpy(output.data(), bytes.data() + offset, output.size()); }
            return true;
        }
        std::span<const std::uint8_t> bytes;
        std::uint64_t total;
    } reader(prefix, totalBytes);
    EncodedInputInspection result;
    PackageInspection inspection;
    if (!InspectPackage(reader, inspection)) {
        result.failure = std::move(inspection.failure);
        return result;
    }
    result.success = true;
    result.byteSize = inspection.byteSize;
    result.version = inspection.version;
    result.contentIdentity = PackageIdentityToHex(inspection.identity);
    result.sourceIdentity = std::move(inspection.sourceIdentity);
    result.kind = inspection.format == PackageBinaryFormat::FramePackage
        ? EncodedPackageKind::Frame : EncodedPackageKind::Leaf;
    return result;
}

}
