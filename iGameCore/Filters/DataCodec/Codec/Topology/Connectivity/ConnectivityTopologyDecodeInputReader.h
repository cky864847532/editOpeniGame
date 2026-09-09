#ifndef DATACODEC_CODEC_TOPOLOGY_CONNECTIVITY_CONNECTIVITYTOPOLOGYDECODEINPUTREADER_H
#define DATACODEC_CODEC_TOPOLOGY_CONNECTIVITY_CONNECTIVITYTOPOLOGYDECODEINPUTREADER_H

#include "DataCodec/Runtime/Cache/CacheResources.h"
#include "DataCodec/Codec/Topology/Connectivity/ConnectivityTopologyTypes.h"
#include "DataCodec/Validation/Common/DataCodecValidation.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace datacodec::topocodec {

// 四段输入由当前块槽位持有，块完成后一起释放
class ConnectivityTopologyDecodeInputReader final : public IConnectivityTopologyEncodedStreamReader {
public:
    ConnectivityTopologyDecodeInputReader() = default;
    ConnectivityTopologyDecodeInputReader(const ConnectivityTopologyDecodeInputReader&) = delete;
    ConnectivityTopologyDecodeInputReader& operator=(const ConnectivityTopologyDecodeInputReader&) = delete;
    ConnectivityTopologyDecodeInputReader(ConnectivityTopologyDecodeInputReader&&) noexcept = default;
    ConnectivityTopologyDecodeInputReader& operator=(ConnectivityTopologyDecodeInputReader&&) noexcept = default;

    template<typename TStream>
    bool LoadFrom(TStream& stream, const ConnectivityTopologyEncodedMetadata& metadata,
        const CacheResources& runtime, std::string* error = nullptr) {
        m_metadata = metadata;
        const std::array<std::uint64_t, 4> sizes{metadata.connectivityByteCount, metadata.cellSizeByteCount,
            metadata.cellPolynomialOrderByteCount, metadata.cellTypeByteCount};
        std::uint64_t total = 0u;
        for (const auto size : sizes) {
            if (!validation::CheckedAddU64(total, size, total, "topology block input size", error) ||
                total > std::numeric_limits<std::size_t>::max()) {
                return validation::AssignError(error, "topology block input exceeds local size capacity");
            }
        }
        for (std::size_t i = 0u; i < sizes.size(); ++i) {
            auto& bytes = m_streams[i];
            bytes.resize(static_cast<std::size_t>(sizes[i]));
            for (std::size_t offset = 0u; offset < bytes.size();) {
                if (runtime.Run().Stopped()) { return false; }
                const auto count = std::min<std::size_t>(kIoWindowBytes, bytes.size() - offset);
                if (!stream.ReadBytes(bytes.data() + offset, count, error)) { return false; }
                offset += count;
            }
        }
        return true;
    }

    [[nodiscard]] const ConnectivityTopologyEncodedMetadata& Metadata() const noexcept override { return m_metadata; }
    void ObserveCapacities(TopologyBlockCapacitySamples& samples) const noexcept {
        samples.Observe(TopologyBufferSample::ConnectivityBytes, m_streams[0]);
        samples.Observe(TopologyBufferSample::CellSizeBytes, m_streams[1]);
        samples.Observe(TopologyBufferSample::PolynomialOrderBytes, m_streams[2]);
        samples.Observe(TopologyBufferSample::CellTypeBytes, m_streams[3]);
    }
    [[nodiscard]] std::uint64_t StreamSize(const ConnectivityTopologyStreamKind kind) const noexcept override {
        const auto i = static_cast<std::size_t>(kind);
        return i < m_streams.size() ? m_streams[i].size() : 0u;
    }
    [[nodiscard]] std::uint64_t ResidentSizeHint() const noexcept override {
        std::uint64_t bytes = 0u;
        for (const auto& stream : m_streams) { bytes = validation::SaturatingAddU64(bytes, stream.capacity()); }
        return bytes;
    }
    [[nodiscard]] std::span<const std::uint8_t> ContiguousStreamRange(
        const ConnectivityTopologyStreamKind kind, const std::uint64_t offset,
        const std::uint64_t count) const noexcept override {
        const auto i = static_cast<std::size_t>(kind);
        if (i >= m_streams.size() || offset > m_streams[i].size() || count > m_streams[i].size() - offset) { return {}; }
        return std::span<const std::uint8_t>(m_streams[i]).subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(count));
    }
    ContiguousViewStatus PrepareContiguousStreamRange(const ConnectivityTopologyStreamKind kind,
        const std::uint64_t offset, const std::uint64_t count,
        std::span<const std::uint8_t>& output, std::string* error = nullptr) const override {
        output = {};
        const auto i = static_cast<std::size_t>(kind);
        if (i >= m_streams.size() || offset > m_streams[i].size() || count > m_streams[i].size() - offset) {
            validation::AssignError(error, "topology block input range is invalid");
            return ContiguousViewStatus::Error;
        }
        output = ContiguousStreamRange(kind, offset, count);
        return ContiguousViewStatus::Ready;
    }
    bool ReadStreamRange(const ConnectivityTopologyStreamKind kind, const std::uint64_t offset,
        const std::span<std::uint8_t> output, std::string* error = nullptr) const override {
        std::span<const std::uint8_t> source;
        if (PrepareContiguousStreamRange(kind, offset, output.size(), source, error) != ContiguousViewStatus::Ready) { return false; }
        if (!output.empty()) { std::memcpy(output.data(), source.data(), output.size()); }
        return true;
    }

private:
    ConnectivityTopologyEncodedMetadata m_metadata{};
    std::array<std::vector<std::uint8_t>, 4> m_streams;
};

} // 拓扑编解码命名空间
#endif
