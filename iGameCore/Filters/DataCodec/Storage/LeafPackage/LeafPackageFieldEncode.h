#ifndef DATACODEC_STORAGE_LEAFPACKAGE_LEAFPACKAGEFIELDENCODE_H
#define DATACODEC_STORAGE_LEAFPACKAGE_LEAFPACKAGEFIELDENCODE_H

#include "DataCodec/Storage/ByteIO/ScratchByteBuffer.h"
#include "DataCodec/Storage/ByteIO/Window/WindowedCopy.h"
#include "DataCodec/Storage/ByteStore/ByteStore.h"
#include "DataCodec/Codec/SubCodec/ZstdCodec.h"
#include "DataCodec/Common/DataCodecTypes.h"
#include "DataCodec/Validation/Common/DataCodecValidation.h"
#include "DataCodec/API/Params/EncodePipelineParams.h"
#include "DataCodec/Runtime/Execution/ParallelExecution.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace datacodec {

struct LeafPackageFieldEncodeRuntime {
    DataCodecExecutionResources& run;
    const HeavyPhaseLease& phase;
    std::function<void(std::uint64_t, std::uint64_t)> progressCallback;
};

namespace detail {

inline constexpr std::uint64_t kPackageFieldZstdProbeThresholdBytes =
    8ull * 1024ull * 1024ull;
inline constexpr std::size_t kPackageFieldZstdProbeBytes =
    1u * 1024u * 1024u;
inline constexpr std::size_t kPackageFieldZstdMinimumSavingsDivisor = 20u;

enum class LeafPackageFieldZstdStrategy {
    Disabled,
    Direct,
    Probe,
};

[[nodiscard]] constexpr LeafPackageFieldZstdStrategy ResolveLeafPackageFieldZstdStrategy(
    const FieldType fieldType,
    const PackageFieldEncodingMode mode,
    const std::uint64_t rawByteSize) noexcept {
    if (mode != PackageFieldEncodingMode::Zstd) {
        return LeafPackageFieldZstdStrategy::Disabled;
    }
    if (rawByteSize < kPackageFieldZstdProbeThresholdBytes) {
        return LeafPackageFieldZstdStrategy::Direct;
    }
    if (fieldType == FieldType::Attribute) {
        return LeafPackageFieldZstdStrategy::Disabled;
    }
    return LeafPackageFieldZstdStrategy::Probe;
}

static_assert(
    ResolveLeafPackageFieldZstdStrategy(
        FieldType::Attribute,
        PackageFieldEncodingMode::Zstd,
        kPackageFieldZstdProbeThresholdBytes) == LeafPackageFieldZstdStrategy::Disabled);
static_assert(
    ResolveLeafPackageFieldZstdStrategy(
        FieldType::Attribute,
        PackageFieldEncodingMode::Zstd,
        kPackageFieldZstdProbeThresholdBytes - 1u) == LeafPackageFieldZstdStrategy::Direct);
static_assert(
    ResolveLeafPackageFieldZstdStrategy(
        FieldType::Topology,
        PackageFieldEncodingMode::Zstd,
        kPackageFieldZstdProbeThresholdBytes) == LeafPackageFieldZstdStrategy::Probe);

class LeafPackageFieldProgressWriter final : public bytestore::IByteWriter {
public:
    LeafPackageFieldProgressWriter(
        bytestore::IByteWriter& downstream,
        const std::uint64_t totalBytes,
        std::function<void(std::uint64_t, std::uint64_t)> callback,
        std::stop_token stop)
        : m_downstream(downstream),
          m_totalBytes(totalBytes),
          m_callback(std::move(callback)), m_stop(stop) {}

    bool Write(
        const std::span<const std::uint8_t> bytes,
        std::string* error = nullptr) override {
        if (m_stop.stop_requested()) {
            return validation::AssignError(error, "package field encoding was stopped");
        }
        if (!m_downstream.Write(bytes, error)) {
            return false;
        }
        m_writtenBytes = std::min<std::uint64_t>(
            m_totalBytes,
            validation::SaturatingAddU64(
                m_writtenBytes,
                static_cast<std::uint64_t>(bytes.size())));
        if (m_callback) {
            m_callback(m_writtenBytes, m_totalBytes);
        }
        return true;
    }

    [[nodiscard]] std::uint64_t ByteSizeHint() const noexcept override {
        return m_downstream.ByteSizeHint();
    }

    [[nodiscard]] std::uint64_t ResidentSizeHint() const noexcept override {
        return m_downstream.ResidentSizeHint();
    }

private:
    bytestore::IByteWriter& m_downstream;
    std::uint64_t m_totalBytes{0u};
    std::uint64_t m_writtenBytes{0u};
    std::function<void(std::uint64_t, std::uint64_t)> m_callback;
    std::stop_token m_stop;
};

inline bool ShouldCompressLeafPackageField(
    bytestore::IByteSource& source,
    const FieldType fieldType,
    const PackageFieldEncodingParams& params,
    const LeafPackageFieldEncodeRuntime& runtime,
    const std::uint64_t rawByteSize,
    bool& shouldCompress,
    std::string* error) {
    const auto strategy = ResolveLeafPackageFieldZstdStrategy(
        fieldType,
        params.mode,
        rawByteSize);
    shouldCompress = strategy != LeafPackageFieldZstdStrategy::Disabled;
    if (strategy != LeafPackageFieldZstdStrategy::Probe) {
        return true;
    }

    const auto probeBytes = static_cast<std::size_t>(std::min<std::uint64_t>(
        rawByteSize,
        static_cast<std::uint64_t>(kPackageFieldZstdProbeBytes)));
    auto probeBuffer = runtime.run.Scratch().Acquire(probeBytes);
    if (!source.Read(0u, probeBuffer.Span(), error)) {
        return false;
    }

    std::vector<std::uint8_t> compressedProbe;
    if (!codec::ZstdCodec::Compress(
            probeBuffer.Span(),
            params.zstdLevel,
            1u,
            compressedProbe,
            error)) {
        return false;
    }
    const auto minimumSavedBytes = std::max<std::size_t>(
        1u,
        probeBytes / kPackageFieldZstdMinimumSavingsDivisor);
    shouldCompress = compressedProbe.size() <= probeBytes - minimumSavedBytes;
    return true;
}

} // namespace detail

inline bool EncodeLeafPackageFieldTerminal(
    bytestore::IByteSource& source,
    const FieldType fieldType,
    const PackageFieldEncodingParams& params,
    const LeafPackageFieldEncodeRuntime& runtime,
    bytestore::IByteWriter& output,
    EncodedFieldCompressionType& compressionType,
    std::uint64_t& rawByteSize,
    const std::size_t computeUnits,
    std::string* error = nullptr) {
    if (runtime.run.Stopped()) {
        return validation::AssignError(error, "package field encoding was stopped");
    }
    rawByteSize = source.ByteSizeHint();
    if (bytestore::IsUnknownByteSize(rawByteSize)) {
        return validation::AssignError(error, "leaf package field source has unknown byte size");
    }
    if (rawByteSize == 0u) {
        compressionType = EncodedFieldCompressionType::None;
        return true;
    }
    if (runtime.progressCallback) {
        runtime.progressCallback(0u, rawByteSize);
    }

    bool shouldCompress = false;
    if (!detail::ShouldCompressLeafPackageField(
            source,
            fieldType,
            params,
            runtime,
            rawByteSize,
            shouldCompress,
            error)) {
        return false;
    }
    compressionType = shouldCompress
        ? EncodedFieldCompressionType::ZSTD
        : EncodedFieldCompressionType::None;
    if (compressionType == EncodedFieldCompressionType::None) {
        detail::LeafPackageFieldProgressWriter progressWriter(
            output,
            rawByteSize,
            runtime.progressCallback,
            runtime.run.StopToken());
        return window::CopyByteSourceByWindow(
            source,
            progressWriter,
            runtime.run.Scratch(),
            error);
    }

    codec::ZstdStreamingEncoder encoder;
    if (!encoder.Initialize(
            params.zstdLevel,
            computeUnits,
            rawByteSize,
            error)) {
        return false;
    }
    codec::ZstdInputWriter zstdWriter(encoder, output);
    detail::LeafPackageFieldProgressWriter progressWriter(
        zstdWriter,
        rawByteSize,
        runtime.progressCallback,
        runtime.run.StopToken());
    if (!window::CopyByteSourceByWindow(
            source,
            progressWriter,
            runtime.run.Scratch(),
            error) ||
        !encoder.Finish(output, error)) {
        return false;
    }
    if (zstdWriter.ByteSizeHint() != rawByteSize) {
        return validation::AssignError(error, "leaf package field raw byte size changed during Zstd encoding");
    }
    return true;
}

inline bool EncodeLeafPackageFieldToWriter(
    bytestore::IByteSource& source,
    const FieldType fieldType,
    const PackageFieldEncodingParams& params,
    const LeafPackageFieldEncodeRuntime& runtime,
    bytestore::IByteWriter& output,
    EncodedFieldCompressionType& compressionType,
    std::uint64_t& rawByteSize,
    std::string* error = nullptr) {
    if (!runtime.phase) {
        return validation::AssignError(error, "package field encoding requires an admitted output phase");
    }
    if (params.mode == PackageFieldEncodingMode::Raw) {
        return EncodeLeafPackageFieldTerminal(source, fieldType, params, runtime, output,
            compressionType, rawByteSize, 1u, error);
    }
    return RunTerminalWork(runtime.run, runtime.phase, [&](WorkerContext& worker) {
        const bool ok = EncodeLeafPackageFieldTerminal(source, fieldType, params, runtime, output,
            compressionType, rawByteSize, worker.ComputeUnits(), error);
        if (!ok && !runtime.run.Stopped()) {
            runtime.run.RecordFailure(MakeCodecFailureRecord(CodecErrorCode::EncodeFailure,
                "package-field-encode", "EncodeLeafPackageFieldToWriter",
                error != nullptr ? std::string_view(*error) : std::string_view("package field encoding failed")));
        }
        return ok;
    }, TerminalWorkKind::ExclusivePackage);
}

} // namespace datacodec

#endif
