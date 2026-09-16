#ifndef DATACODEC_STORAGE_PACKAGE_PACKAGEBINARYHEADER_H
#define DATACODEC_STORAGE_PACKAGE_PACKAGEBINARYHEADER_H

#include "DataCodec/Storage/ByteIO/ByteRange.h"
#include "DataCodec/Storage/Common/BinaryValueIO.h"
#include "DataCodec/Storage/FramePackage/FramePackageWireLayout.h"
#include "DataCodec/Storage/LeafPackage/LeafPackageWireLayout.h"
#include "DataCodec/Storage/Package/PackageIdentity.h"
#include "DataCodec/Common/DataCodecError.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace datacodec {

enum class PackageBinaryFormat {
    Unknown,
    LeafPackage,
    FramePackage,
};

struct PackageInspection {
    PackageBinaryFormat format{PackageBinaryFormat::Unknown};
    std::uint32_t magic{0u};
    std::uint16_t version{0u};
    PackageIdentity identity;
    std::uint64_t byteSize{0u};
    DecodeSourceIdentity sourceIdentity;
    std::optional<CodecFailureRecord> failure;
};

inline bool InspectPackage(
    IByteRangeReader& reader,
    PackageInspection& inspection,
    std::string* error = nullptr,
    const std::stop_token stop = {}) {
    constexpr std::size_t kFixedHeaderByteCount =
        sizeof(std::uint32_t) + sizeof(std::uint16_t) + sizeof(std::uint64_t) * 2u;
    inspection = {};
    inspection.byteSize = reader.ByteSize();
    const auto fail = [&](CodecErrorCode code, const char* message) {
        inspection.failure = MakeCodecFailureRecord(code, "package.header", "InspectPackage", message);
        if (error != nullptr) { *error = message; }
        return false;
    };
    if (inspection.byteSize < kFixedHeaderByteCount) {
        return fail(CodecErrorCode::IncompleteInput, "package header is incomplete");
    }

    std::array<std::uint8_t, kFixedHeaderByteCount> bytes{};
    if (!reader.ReadAtCancellable(
            0u,
            std::span<std::uint8_t>(bytes),
            stop,
            error)) {
        inspection.failure = MakeCodecFailureRecord(CodecErrorCode::IncompleteInput,
            "package.read", "InspectPackage", error ? *error : "package header read failed", stop.stop_requested());
        return false;
    }
    std::size_t cursor = 0u;
    if (!detail::ReadScalar(
            std::span<const std::uint8_t>(bytes),
            cursor,
            inspection.magic,
            error) ||
        !detail::ReadScalar(
            std::span<const std::uint8_t>(bytes),
            cursor,
            inspection.version,
            error) ||
        !detail::ReadScalar(
            std::span<const std::uint8_t>(bytes),
            cursor,
            inspection.identity.high,
            error) ||
        !detail::ReadScalar(
            std::span<const std::uint8_t>(bytes),
            cursor,
            inspection.identity.low,
            error)) {
        return fail(CodecErrorCode::IncompleteInput, "package header is incomplete");
    }

    if (inspection.magic == leafpackagewire::kLeafPackageMagic) {
        inspection.format = PackageBinaryFormat::LeafPackage;
    } else if (inspection.magic == framepackagewire::kFramePackageMagic) {
        inspection.format = PackageBinaryFormat::FramePackage;
    } else {
        return fail(CodecErrorCode::InvalidFormat, "package magic is invalid");
    }
    const auto expectedVersion = inspection.format == PackageBinaryFormat::LeafPackage
        ? leafpackagewire::kLeafPackageVersion
        : framepackagewire::kFramePackageVersion;
    if (inspection.version != expectedVersion) {
        fail(CodecErrorCode::UnsupportedVersion, "package version is unsupported");
        inspection.failure->actualVersion = inspection.version;
        inspection.failure->supportedVersion = expectedVersion;
        return false;
    }
    if (!inspection.identity.IsValid()) {
        return fail(CodecErrorCode::InvalidFormat, "package identity is invalid");
    }
    inspection.sourceIdentity = MakePackageDecodeSourceIdentity(
        inspection.identity,
        inspection.version,
        inspection.byteSize);
    if (!inspection.sourceIdentity.IsStable()) {
        return fail(CodecErrorCode::InvalidFormat, "package identity is invalid");
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

} // namespace datacodec

#endif
