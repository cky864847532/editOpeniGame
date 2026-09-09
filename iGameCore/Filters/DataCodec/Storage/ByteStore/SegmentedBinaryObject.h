#ifndef DATACODEC_STORAGE_BYTESTORE_SEGMENTEDBINARYOBJECT_H
#define DATACODEC_STORAGE_BYTESTORE_SEGMENTEDBINARYOBJECT_H

#include "DataCodec/Storage/ByteIO/ByteSource.h"
#include "DataCodec/Validation/Common/DataCodecValidation.h"
#include "DataCodec/Common/DataCodecTypes.h"
#include "DataCodec/Storage/ByteIO/ByteRange.h"
#include "DataCodec/Storage/ByteStore/ByteStore.h"
#include "DataCodec/Runtime/Execution/ParallelExecution.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>
namespace datacodec {
namespace bytestore {

class SegmentedBinaryObject final : public IByteSource {
public:
    struct Segment {
        std::shared_ptr<IByteSource> source;
        std::uint64_t byteSize{0u};
    };

    explicit SegmentedBinaryObject(
        std::vector<Segment> segments = {},
        const ByteSourceConsumptionMode replayMode = ByteSourceConsumptionMode::OneShot)
        : m_segments(std::move(segments)),
          m_replayMode(replayMode) {
        RecomputeByteSize();
    }

    SegmentedBinaryObject(const SegmentedBinaryObject&) = delete;
    SegmentedBinaryObject& operator=(const SegmentedBinaryObject&) = delete;

    ~SegmentedBinaryObject() override = default;

    bool AddSegment(
        std::shared_ptr<IByteSource> source,
        std::string* error = nullptr) {
        if (m_consumed && m_replayMode == ByteSourceConsumptionMode::OneShot) {
            return validation::AssignError(error, "one-shot segmented binary object was already consumed");
        }
        if (source == nullptr) {
            return validation::AssignError(error, "segmented binary object segment source is null");
        }
        const auto byteSize = source->ByteSizeHint();
        if (IsUnknownByteSize(byteSize)) {
            return validation::AssignError(error, "segmented binary object segment size is unknown");
        }
        if (!validation::CanAddU64(m_byteSize, byteSize)) {
            validation::AssignError(error, "segmented binary object byte size overflows");
            return false;
        }
        m_byteSize += byteSize;
        m_segments.push_back(Segment{
            .source = std::move(source),
            .byteSize = byteSize,
        });
        return true;
    }

    [[nodiscard]] std::size_t SegmentCount() const noexcept {
        return m_segments.size();
    }

    [[nodiscard]] const std::shared_ptr<IByteSource>& SegmentSource(
        const std::size_t index) const noexcept {
        return m_segments[index].source;
    }

    [[nodiscard]] std::uint64_t SegmentByteSize(const std::size_t index) const noexcept {
        return m_segments[index].byteSize;
    }

    [[nodiscard]] std::uint64_t ByteSizeHint() const noexcept override {
        return m_byteSize;
    }

    [[nodiscard]] std::uint64_t ResidentSizeHint() const noexcept override {
        std::uint64_t residentBytes = VectorCapacityBytes(m_segments);
        for (const auto& segment : m_segments) {
            if (segment.source != nullptr) {
                residentBytes = validation::SaturatingAddU64(residentBytes, segment.source->ResidentSizeHint());
            }
        }
        return residentBytes;
    }

    [[nodiscard]] std::uint64_t MappedSizeHint() const noexcept override {
        std::uint64_t mappedBytes = 0u;
        for (const auto& segment : m_segments) {
            if (segment.source != nullptr) {
                mappedBytes = validation::SaturatingAddU64(mappedBytes, segment.source->MappedSizeHint());
            }
        }
        return mappedBytes;
    }

    [[nodiscard]] bool CanRead() const noexcept override {
        if (m_consumed && m_replayMode == ByteSourceConsumptionMode::OneShot) {
            return false;
        }
        if (m_segments.empty()) {
            return true;
        }
        for (const auto& segment : m_segments) {
            if (segment.source == nullptr || !segment.source->CanRead()) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool PreferDirectCopy() const noexcept override { return true; }

    bool Read(
        const std::uint64_t offset,
        const std::span<std::uint8_t> output,
        std::string* error = nullptr) const override {
        if (!CanRead()) {
            return validation::AssignError(error, "segmented binary object is not readable");
        }
        if (offset > m_byteSize || output.size() > m_byteSize - offset) {
            return validation::AssignError(error, "segmented binary object read is outside the object range");
        }
        const auto requestEnd = offset + static_cast<std::uint64_t>(output.size());
        std::uint64_t segmentOffset = 0u;
        std::size_t outputOffset = 0u;
        for (const auto& segment : m_segments) {
            const auto segmentBegin = segmentOffset;
            const auto segmentEnd = segmentBegin + segment.byteSize;
            segmentOffset = segmentEnd;
            if (segment.byteSize == 0u || requestEnd <= segmentBegin || offset >= segmentEnd) {
                continue;
            }
            const auto readBegin = std::max(offset, segmentBegin);
            const auto readEnd = std::min(requestEnd, segmentEnd);
            const auto readBytes = static_cast<std::size_t>(readEnd - readBegin);
            const auto sourceOffset = readBegin - segmentBegin;
            if (!segment.source->Read(
                    sourceOffset,
                    output.subspan(outputOffset, readBytes),
                    error)) {
                return false;
            }
            outputOffset += readBytes;
        }
        return outputOffset == output.size();
    }

    bool CopyTo(IByteWriter& writer, std::string* error = nullptr) override {
        if (!CanRead()) {
            return validation::AssignError(error, "segmented binary object is not readable for copy");
        }
        for (auto& segment : m_segments) {
            if (segment.source != nullptr && !segment.source->CopyTo(writer, error)) {
                return false;
            }
            if (m_replayMode == ByteSourceConsumptionMode::OneShot && segment.source != nullptr) {
                segment.source.reset();
            }
        }
        if (m_replayMode == ByteSourceConsumptionMode::OneShot) {
            m_consumed = true;
        }
        return true;
    }

    [[nodiscard]] bool Materialize(DataCodecExecutionResources& run, EncodedBuffer& bytes,
                                   std::string* error = nullptr) {
        bytes = {};
        if (!CanRead()) { return validation::AssignError(error, "segmented object is not readable"); }
        auto phase = WaitForHeavyPhase(run);
        if (!phase) { return false; }
        MemoryByteRangeOutput output(run);
        KnownStorageOwners coexist;
        for (const auto& segment : m_segments) { coexist.Add(segment.source.get()); }
        // 完整目标先取得容量，成功后才允许一次性分段被消费
        if (!output.PrepareExactSize(m_byteSize, error, coexist.Entries())) { return false; }
        class MaterializeWriter final : public IByteWriter {
        public:
            MaterializeWriter(MemoryByteRangeOutput& output, DataCodecExecutionResources& run)
                : m_output(output), m_run(run) {}
            bool Write(std::span<const std::uint8_t> data, std::string* error) override {
                while (!data.empty()) {
                    if (m_run.Stopped()) { return validation::AssignError(error, "materialization was stopped"); }
                    const auto count = std::min<std::size_t>(data.size(), kIoWindowBytes);
                    if (!m_output.WriteAt(m_offset, data.first(count), error)) { return false; }
                    m_offset += count;
                    data = data.subspan(count);
                }
                return true;
            }
            std::uint64_t ByteSizeHint() const noexcept override { return m_offset; }
        private:
            MemoryByteRangeOutput& m_output;
            DataCodecExecutionResources& m_run;
            std::uint64_t m_offset{0u};
        } writer(output, run);
        if (!CopyTo(writer, error) || writer.ByteSizeHint() != m_byteSize ||
            !output.Finalize(m_byteSize, error)) { return false; }
        bytes = output.TakeBytes();
        return true;
    }

private:
    void RecomputeByteSize() noexcept {
        m_byteSize = 0u;
        for (auto& segment : m_segments) {
            if (segment.source != nullptr) {
                segment.byteSize = segment.source->ByteSizeHint();
                if (!IsUnknownByteSize(segment.byteSize) &&
                    validation::CanAddU64(m_byteSize, segment.byteSize)) {
                    m_byteSize += segment.byteSize;
                }
            }
        }
    }

    std::vector<Segment> m_segments;
    std::uint64_t m_byteSize{0u};
    ByteSourceConsumptionMode m_replayMode{ByteSourceConsumptionMode::OneShot};
    bool m_consumed{false};
};

inline std::shared_ptr<SegmentedBinaryObject> MakeSegmentedBinaryObject(
    const std::span<const std::shared_ptr<IByteSource>> sources,
    std::string* error = nullptr,
    const ByteSourceConsumptionMode replayMode = ByteSourceConsumptionMode::OneShot) {
    auto object = std::make_shared<SegmentedBinaryObject>(
        std::vector<SegmentedBinaryObject::Segment>{},
        replayMode);
    for (const auto& source : sources) {
        if (!object->AddSegment(source, error)) {
            return nullptr;
        }
    }
    return object;
}

} // namespace bytestore
} // namespace datacodec

#endif
