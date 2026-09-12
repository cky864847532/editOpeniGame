#ifndef DATACODEC_CODEC_REFERENCE_ATTRIBUTEREFERENCESCHEDULEBUILDER_H
#define DATACODEC_CODEC_REFERENCE_ATTRIBUTEREFERENCESCHEDULEBUILDER_H

#include "DataCodec/Codec/Reference/AttributeReferenceSchedule.h"
#include "DataCodec/Storage/ByteStore/ByteStore.h"
#include "DataCodec/Runtime/Execution/ParallelExecution.h"
#include "DataCodec/Codec/Reference/IntraFieldReference.h"
#include "DataCodec/Codec/NumericArray/IntegerResidualCodec.h"
#include "DataCodec/Codec/NumericArray/NumericArrayReader.h"
#include "DataCodec/Codec/Reference/TemporalReference.h"
#include "DataCodec/Common/DataCodecTypes.h"
#include "DataCodec/Validation/Common/DataCodecValidation.h"
#include "DataCodec/API/Params/ReferenceControlParams.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
namespace datacodec {

inline bool IsReferenceEligibleAttributeField(const AttrStorageParams& meta) {
    const auto expectedValueSize = DataTypeSize(meta.dataType);
    const auto isFloat =
        (meta.dataType == DataType::Float32 || meta.dataType == DataType::Float64) &&
        (NumericArrayValueSize(meta) == sizeof(float) ||
            NumericArrayValueSize(meta) == sizeof(double));
    const auto isInteger =
        numericarray::IsIntegerNumericArrayDataType(meta.dataType) &&
        expectedValueSize != 0u &&
        NumericArrayValueSize(meta) == expectedValueSize;
    return (isFloat || isInteger) &&
        meta.dimension > 0 &&
        meta.elementCount > 0;
}

inline bool HasMatchingAttributeReferenceSampleLayout(
    const AttrStorageParams& lhs,
    const AttrStorageParams& rhs) noexcept {
    return lhs.dataType == rhs.dataType &&
        lhs.elementCount == rhs.elementCount &&
        lhs.dimension == rhs.dimension;
}

inline bool IsAttributeReferenceSampleFieldEligible(
    const AttrStorageParams& meta,
    const IntraFieldReferenceCodec codec) noexcept {
    if (!IsReferenceEligibleAttributeField(meta)) {
        return false;
    }
    if (numericarray::IsIntegerNumericArrayDataType(meta.dataType)) {
        return codec == IntraFieldReferenceCodec::Wavelet;
    }
    return codec == IntraFieldReferenceCodec::Affine ||
        codec == IntraFieldReferenceCodec::Predictor ||
        codec == IntraFieldReferenceCodec::Wavelet;
}

template<class T>
inline bool CreateAttributeReferenceArray(
    bytestore::ByteStoreSession& session, const std::size_t count, const char* label,
    std::shared_ptr<bytestore::MemoryStore>& owner, std::span<T>& values, std::string* error) {
    owner.reset();
    values = {};
    std::size_t bytes = 0u;
    if (!validation::CheckedMulSizeT(count, sizeof(T), bytes, label, error)) { return false; }
    owner = std::static_pointer_cast<bytestore::MemoryStore>(
        session.CreateSizedStore(bytestore::ByteStorePurpose::Contiguous, bytes, ::datacodec::MemoryDemandKind::RequiredContinuation, label, error));
    if (!owner) { return false; }
    const auto storage = owner->WritableBytes();
    if (storage.size() != bytes || (bytes != 0u &&
            reinterpret_cast<std::uintptr_t>(storage.data()) % alignof(T) != 0u)) {
        return validation::AssignError(error, "reference schedule array is not aligned complete storage");
    }
    values = {reinterpret_cast<T*>(storage.data()), count};
    return true;
}

inline bool BuildAttributeReferenceSampleIndices(
    const ParamSize elementCountParam, const std::size_t requestedSampleCount,
    std::span<std::size_t>& indices, std::string* error = nullptr) {
    std::size_t elementCount = 0u;
    if (!TryParamSizeToSizeT(elementCountParam, elementCount)) {
        return validation::AssignError(error, "attribute reference sample element count exceeds this platform size limit");
    }
    const auto sampleCount = std::min(elementCount, std::max<std::size_t>(1u, requestedSampleCount));
    if (indices.size() != sampleCount) {
        return validation::AssignError(error, "attribute sample index storage does not match its admitted capacity");
    }
    if (sampleCount == elementCount) {
        for (std::size_t i = 0u; i < elementCount; ++i) { indices[i] = i; }
        return true;
    }
    if (sampleCount == 1u) { indices[0u] = 0u; return true; }
    const auto lastIndex = static_cast<long double>(elementCount - 1u);
    const auto denominator = static_cast<long double>(sampleCount - 1u);
    for (std::size_t i = 0u; i < sampleCount; ++i) {
        const auto resolved = static_cast<std::size_t>(std::llround(
            lastIndex * static_cast<long double>(i) / denominator));
        indices[i] = std::min(resolved, elementCount - 1u);
    }
    const auto last = std::unique(indices.begin(), indices.end());
    indices = indices.first(static_cast<std::size_t>(last - indices.begin()));
    return true;
}

struct AttributeReferenceFieldSample {
    std::shared_ptr<bytestore::MemoryStore> owner;
    std::span<std::uint8_t> bytes;
    std::size_t tupleCount{0u};
    std::size_t componentCount{0u};
    std::size_t valueSize{0u};
};

struct AttributeReferenceSampleGroup {
    std::size_t representativeFieldIndex{0u};
    std::vector<std::size_t> fieldIndices;
};

inline bool CalculateAttributeReferenceFieldSampleBytes(
    const AttrStorageParams& meta, const std::size_t sampleCount,
    std::size_t& sampleBytes, std::string* error = nullptr) {
    std::size_t valueSize = 0u, tupleBytes = 0u;
    if (!TryParamSizeToSizeT(NumericArrayValueSize(meta), valueSize)) {
        return validation::AssignError(error, "attribute reference sample value size exceeds this platform size limit");
    }
    return validation::CheckedMulSizeT(static_cast<std::size_t>(std::max(meta.dimension, 0)),
        valueSize, tupleBytes, "attribute reference sample tuple bytes", error) &&
        validation::CheckedMulSizeT(sampleCount, tupleBytes, sampleBytes,
            "attribute reference sample bytes", error);
}

inline bool PrepareAttributeReferenceFieldSample(
    const AttrStorageParams& meta, const std::size_t sampleCount, bytestore::ByteStoreSession& session,
    AttributeReferenceFieldSample& sample, std::string* error) {
    sample = {};
    if (!TryParamSizeToSizeT(NumericArrayValueSize(meta), sample.valueSize)) {
        return validation::AssignError(error, "attribute reference sample value size exceeds this platform size limit");
    }
    sample.componentCount = static_cast<std::size_t>(std::max(meta.dimension, 0));
    sample.tupleCount = sampleCount;
    std::size_t sampleBytes = 0u;
    if (!CalculateAttributeReferenceFieldSampleBytes(meta, sampleCount, sampleBytes, error)) { return false; }
    return CreateAttributeReferenceArray(session, sampleBytes, "attribute_reference_sample",
        sample.owner, sample.bytes, error);
}

inline bool ReadAttributeReferenceFieldSample(
    const numericarray::NumericArraySource& source, const std::span<const std::size_t> sampleIndices,
    ScratchByteBufferPool& scratchBytePool, AttributeReferenceFieldSample& sample, std::string* error) {
    numericarray::NumericArrayReader reader;
    if (!numericarray::BuildNumericArrayReader(source, reader, error)) { return false; }
    const auto tupleBytes = sample.componentCount * sample.valueSize;
    if (tupleBytes == 0u || sample.tupleCount != sampleIndices.size()) {
        return validation::AssignError(error, "attribute reference sample layout is not prepared");
    }
    const auto windowCount = std::max<std::size_t>(1u, kIoWindowBytes / tupleBytes);
    std::size_t cursor = 0u;
    while (cursor < sampleIndices.size()) {
        const auto runStart = sampleIndices[cursor];
        std::size_t runCount = 1u;
        while (runCount < windowCount && cursor + runCount < sampleIndices.size() &&
               sampleIndices[cursor + runCount] == runStart + runCount) { ++runCount; }
        ScratchByteBuffer range;
        if (!reader.ReadElements(runStart, runCount, scratchBytePool, range, error)) { return false; }
        if (range.Span().size() != runCount * tupleBytes) {
            return validation::AssignError(error, "attribute sample read returned an invalid byte count");
        }
        std::memcpy(sample.bytes.data() + cursor * tupleBytes, range.Span().data(), range.Span().size());
        cursor += runCount;
    }
    return sample.owner->Seal(error);
}

template<typename TValue>
inline TValue ReadAttributeReferenceSampleValue(
    const AttributeReferenceFieldSample& sample,
    const std::size_t tupleIndex,
    const std::size_t componentIndex) noexcept {
    TValue value{};
    const auto valueIndex = tupleIndex * sample.componentCount + componentIndex;
    std::memcpy(
        &value,
        sample.bytes.data() + valueIndex * sizeof(TValue),
        sizeof(TValue));
    return value;
}

template<typename TValue>
inline double ComputeAffineAttributeReferenceSampleScore(
    const AttributeReferenceFieldSample& current,
    const AttributeReferenceFieldSample& reference) noexcept {
    constexpr long double kEpsilon = 1.0e-18L;
    long double totalScore = 0.0L;
    std::size_t validComponentCount = 0u;
    for (std::size_t componentIndex = 0u;
         componentIndex < current.componentCount;
         ++componentIndex) {
        long double sumX = 0.0L;
        long double sumY = 0.0L;
        long double sumXX = 0.0L;
        long double sumYY = 0.0L;
        long double sumXY = 0.0L;
        std::size_t count = 0u;
        for (std::size_t tupleIndex = 0u; tupleIndex < current.tupleCount; ++tupleIndex) {
            const auto x = static_cast<long double>(
                ReadAttributeReferenceSampleValue<TValue>(reference, tupleIndex, componentIndex));
            const auto y = static_cast<long double>(
                ReadAttributeReferenceSampleValue<TValue>(current, tupleIndex, componentIndex));
            if (!std::isfinite(static_cast<double>(x)) ||
                !std::isfinite(static_cast<double>(y))) {
                continue;
            }
            sumX += x;
            sumY += y;
            sumXX += x * x;
            sumYY += y * y;
            sumXY += x * y;
            ++count;
        }
        if (count == 0u) {
            continue;
        }
        const auto localCount = static_cast<long double>(count);
        const auto sxx = sumXX - sumX * sumX / localCount;
        const auto syy = sumYY - sumY * sumY / localCount;
        const auto sxy = sumXY - sumX * sumY / localCount;
        if (syy <= kEpsilon) {
            totalScore += 1.0L;
            ++validComponentCount;
            continue;
        }
        if (sxx <= kEpsilon) {
            continue;
        }
        totalScore += std::clamp((sxy * sxy) / (sxx * syy), 0.0L, 1.0L);
        ++validComponentCount;
    }
    return validComponentCount == 0u
        ? 0.0
        : static_cast<double>(totalScore / static_cast<long double>(validComponentCount));
}

template<typename TValue>
inline double ComputeDeltaAttributeReferenceSampleScore(
    const AttributeReferenceFieldSample& current,
    const AttributeReferenceFieldSample& reference) noexcept {
    constexpr long double kEpsilon = 1.0e-18L;
    long double totalScore = 0.0L;
    std::size_t validComponentCount = 0u;
    for (std::size_t componentIndex = 0u;
         componentIndex < current.componentCount;
         ++componentIndex) {
        long double sumCurrent = 0.0L;
        long double sumResidual = 0.0L;
        long double sumCurrentSquared = 0.0L;
        long double sumResidualSquared = 0.0L;
        std::size_t count = 0u;
        for (std::size_t tupleIndex = 0u; tupleIndex < current.tupleCount; ++tupleIndex) {
            const auto currentValue = static_cast<long double>(
                ReadAttributeReferenceSampleValue<TValue>(current, tupleIndex, componentIndex));
            const auto referenceValue = static_cast<long double>(
                ReadAttributeReferenceSampleValue<TValue>(reference, tupleIndex, componentIndex));
            if (!std::isfinite(static_cast<double>(currentValue)) ||
                !std::isfinite(static_cast<double>(referenceValue))) {
                continue;
            }
            const auto residual = currentValue - referenceValue;
            sumCurrent += currentValue;
            sumResidual += residual;
            sumCurrentSquared += currentValue * currentValue;
            sumResidualSquared += residual * residual;
            ++count;
        }
        if (count == 0u) {
            continue;
        }
        const auto localCount = static_cast<long double>(count);
        const auto currentVariance =
            sumCurrentSquared - sumCurrent * sumCurrent / localCount;
        const auto residualVariance =
            sumResidualSquared - sumResidual * sumResidual / localCount;
        long double componentScore = 0.0L;
        if (residualVariance <= kEpsilon) {
            componentScore = 1.0L;
        } else if (currentVariance > kEpsilon) {
            componentScore = std::clamp(
                1.0L - residualVariance / currentVariance,
                0.0L,
                1.0L);
        }
        totalScore += componentScore;
        ++validComponentCount;
    }
    return validComponentCount == 0u
        ? 0.0
        : static_cast<double>(totalScore / static_cast<long double>(validComponentCount));
}

inline std::size_t AttributeReferenceResidualBitWidth(std::uint64_t value) noexcept {
    std::size_t bitWidth = 0u;
    while (value != 0u) {
        ++bitWidth;
        value >>= 1u;
    }
    return bitWidth;
}

inline double ComputeIntegerAttributeReferenceSampleScore(
    const AttrStorageParams& meta,
    const AttributeReferenceFieldSample& current,
    const AttributeReferenceFieldSample& reference) noexcept {
    const auto storageBitWidth = current.valueSize * 8u;
    const auto valueCount = current.tupleCount * current.componentCount;
    if (storageBitWidth == 0u || valueCount == 0u) {
        return 0.0;
    }
    const auto mask = numericarray::IntegerStorageMask(current.valueSize);
    long double totalResidualBitWidth = 0.0L;
    for (std::size_t valueIndex = 0u; valueIndex < valueCount; ++valueIndex) {
        const auto byteOffset = valueIndex * current.valueSize;
        const auto currentRaw = numericarray::ReadIntegerStorageValue(
            current.bytes.data() + byteOffset,
            current.valueSize);
        const auto referenceRaw = numericarray::ReadIntegerStorageValue(
            reference.bytes.data() + byteOffset,
            reference.valueSize);
        const auto currentCode = numericarray::ToIntegerOrderCode(
            meta.dataType,
            currentRaw,
            current.valueSize);
        const auto referenceCode = numericarray::ToIntegerOrderCode(
            meta.dataType,
            referenceRaw,
            reference.valueSize);
        const auto delta = (currentCode - referenceCode) & mask;
        const auto zigZag = numericarray::SignedModuloToZigZag(delta, current.valueSize);
        totalResidualBitWidth += static_cast<long double>(
            AttributeReferenceResidualBitWidth(zigZag));
    }
    const auto maximumBitWidth =
        static_cast<long double>(valueCount) * static_cast<long double>(storageBitWidth);
    return static_cast<double>(std::clamp(
        1.0L - totalResidualBitWidth / maximumBitWidth,
        0.0L,
        1.0L));
}

inline bool ComputeAttributeReferenceSampleScore(
    const AttrStorageParams& meta,
    const IntraFieldReferenceCodec codec,
    const AttributeReferenceFieldSample& current,
    const AttributeReferenceFieldSample& reference,
    double& score,
    std::string* error = nullptr) {
    score = 0.0;
    if (current.tupleCount != reference.tupleCount ||
        current.componentCount != reference.componentCount ||
        current.valueSize != reference.valueSize ||
        current.bytes.size() != reference.bytes.size()) {
        return validation::AssignError(error, "attribute reference samples do not share a common layout");
    }
    if (numericarray::IsIntegerNumericArrayDataType(meta.dataType)) {
        if (codec != IntraFieldReferenceCodec::Wavelet) {
            return validation::AssignError(error, "integer attribute reference sampling requires wavelet codec");
        }
        score = ComputeIntegerAttributeReferenceSampleScore(meta, current, reference);
        return true;
    }
    if (meta.dataType == DataType::Float32 && NumericArrayValueSize(meta) == sizeof(float)) {
        score = codec == IntraFieldReferenceCodec::Affine
            ? ComputeAffineAttributeReferenceSampleScore<float>(current, reference)
            : ComputeDeltaAttributeReferenceSampleScore<float>(current, reference);
        return true;
    }
    if (meta.dataType == DataType::Float64 && NumericArrayValueSize(meta) == sizeof(double)) {
        score = codec == IntraFieldReferenceCodec::Affine
            ? ComputeAffineAttributeReferenceSampleScore<double>(current, reference)
            : ComputeDeltaAttributeReferenceSampleScore<double>(current, reference);
        return true;
    }
    return validation::AssignError(error, "attribute reference sampling uses an unsupported numeric type");
}

inline double ResolveAttributeReferenceMinimumSampleScore(
    const IntraFieldReferenceControlParams& control) noexcept {
    return control.codec == IntraFieldReferenceCodec::Affine
        ? control.affine.precheckRSquared
        : control.minimumSampleScore;
}

inline bool BuildAttributeIntraFieldReferenceSchedule(
    const std::vector<AttrStorageParams>& metas,
    const std::vector<numericarray::NumericArraySource>& sources,
    const std::vector<std::size_t>& metaIndices,
    const std::vector<std::uint8_t>& referenceAllowed,
    const AttrReferenceControlParams& dependency,
    DataCodecExecutionResources& resources,
    bytestore::ByteStoreSession& byteStoreSession,
    EncodeAttributeReferenceSchedule& schedule,
    std::string* error = nullptr) {
    const auto attrCount = metas.size();
    if (sources.size() != attrCount ||
        metaIndices.size() != attrCount ||
        referenceAllowed.size() != attrCount) {
        return validation::AssignError(error, "attribute reference schedule input sizes do not match");
    }
    schedule.initialized = false;
    schedule.topologyOrder.resize(attrCount);
    schedule.entries.clear();
    schedule.entries.resize(attrCount);
    for (std::size_t index = 0; index < attrCount; ++index) {
        schedule.topologyOrder[index] = index;
    }
    if (attrCount == 0u || !IsIntraFieldReferenceEnabled(dependency)) {
        schedule.initialized = true;
        return true;
    }

    const auto minimumSampleScore = ResolveAttributeReferenceMinimumSampleScore(
        dependency.intraField);
    if (!std::isfinite(minimumSampleScore) ||
        minimumSampleScore < 0.0 ||
        minimumSampleScore > 1.0) {
        return validation::AssignError(
            error,
            "attribute reference minimum sample score must be within [0, 1]");
    }

    std::vector<AttributeReferenceSampleGroup> sampleGroups;
    for (std::size_t fieldIndex = 0u; fieldIndex < attrCount; ++fieldIndex) {
        if (!IsAttributeReferenceSampleFieldEligible(
                metas[fieldIndex],
                dependency.intraField.codec)) {
            continue;
        }
        auto groupIt = std::find_if(
            sampleGroups.begin(),
            sampleGroups.end(),
            [&](const AttributeReferenceSampleGroup& group) {
                return HasMatchingAttributeReferenceSampleLayout(
                    metas[group.representativeFieldIndex],
                    metas[fieldIndex]);
            });
        if (groupIt == sampleGroups.end()) {
            AttributeReferenceSampleGroup group;
            group.representativeFieldIndex = fieldIndex;
            group.fieldIndices.push_back(fieldIndex);
            sampleGroups.push_back(std::move(group));
            continue;
        }
        groupIt->fieldIndices.push_back(fieldIndex);
    }

    auto phase = WaitForHeavyPhase(resources);
    if (!phase) { return false; }
    std::shared_ptr<bytestore::MemoryStore> edgeOwner;
    std::span<IntraFieldEdge> edges;
    if (!CreateAttributeReferenceArray(byteStoreSession, 0u, "attribute_reference_edges",
            edgeOwner, edges, error)) { return false; }
    for (const auto& group : sampleGroups) {
        if (group.fieldIndices.size() < 2u) { continue; }
        if (resources.Stopped()) { return false; }
        std::size_t elementCount = 0u;
        if (!TryParamSizeToSizeT(metas[group.representativeFieldIndex].elementCount, elementCount)) {
            return validation::AssignError(error, "attribute sample domain exceeds this platform size limit");
        }
        const auto sampleCapacity = std::min(elementCount,
            std::max<std::size_t>(1u, dependency.intraField.sampleCount));
        std::shared_ptr<bytestore::MemoryStore> indexOwner;
        std::span<std::size_t> sampleIndices;
        if (!CreateAttributeReferenceArray(byteStoreSession, sampleCapacity, "attribute_sample_indices",
                indexOwner, sampleIndices, error)) { return false; }
        if (!RunTerminalWork(resources, *phase, [&](WorkerContext&) {
                return BuildAttributeReferenceSampleIndices(elementCount, dependency.intraField.sampleCount,
                    sampleIndices, error);
            })) { return false; }
        if (!indexOwner->Seal(error)) { return false; }

        std::vector<AttributeReferenceFieldSample> fieldSamples(group.fieldIndices.size());
        for (std::size_t local = 0u; local < group.fieldIndices.size(); ++local) {
            if (!PrepareAttributeReferenceFieldSample(metas[group.fieldIndices[local]], sampleIndices.size(),
                    byteStoreSession, fieldSamples[local], error)) { return false; }
        }
        if (!RunTerminalWork(resources, *phase, [&](WorkerContext& worker) {
                for (std::size_t local = 0u; local < group.fieldIndices.size(); ++local) {
                    if (worker.StopToken().stop_requested() ||
                        !ReadAttributeReferenceFieldSample(sources[group.fieldIndices[local]], sampleIndices,
                            worker.Scratch(), fieldSamples[local], error)) { return false; }
                }
                for (std::size_t childLocal = 0u; childLocal < group.fieldIndices.size(); ++childLocal) {
                    const auto child = group.fieldIndices[childLocal];
                    if (referenceAllowed[child] == 0u) { continue; }
                    for (std::size_t parentLocal = 0u; parentLocal < group.fieldIndices.size(); ++parentLocal) {
                        if (worker.StopToken().stop_requested()) { return false; }
                        const auto parent = group.fieldIndices[parentLocal];
                        if (parent == child) { continue; }
                        double score = 0.0;
                        if (!ComputeAttributeReferenceSampleScore(metas[child], dependency.intraField.codec,
                                fieldSamples[childLocal], fieldSamples[parentLocal], score, error)) { return false; }
                        if (dependency.intraField.selectionMode != ReferenceSelectionMode::Forced &&
                            score < minimumSampleScore) { continue; }
                        const IntraFieldEdge edge{.parent = parent, .child = child, .score = score};
                        // Append 直接预约真实增长容量，旧新存储并存由同一根容量事务覆盖
                        if (!edgeOwner->Append({reinterpret_cast<const std::uint8_t*>(&edge), sizeof(edge)}, error)) {
                            return false;
                        }
                    }
                }
                return true;
            })) { return false; }
        // 当前组的样本与索引在下一组准入前释放，真实候选边继续持有容量
    }
    const auto edgeBytes = edgeOwner->WritableBytes();
    if (edgeBytes.size() % sizeof(IntraFieldEdge) != 0u || (!edgeBytes.empty() &&
            reinterpret_cast<std::uintptr_t>(edgeBytes.data()) % alignof(IntraFieldEdge) != 0u)) {
        return validation::AssignError(error, "attribute reference edge storage has an invalid layout");
    }
    edges = {reinterpret_cast<IntraFieldEdge*>(edgeBytes.data()), edgeBytes.size() / sizeof(IntraFieldEdge)};
    const bool selected = RunTerminalWork(resources, *phase, [&](WorkerContext&) {
    std::sort(edges.begin(), edges.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.score != rhs.score) {
            return lhs.score > rhs.score;
        }
        if (lhs.child != rhs.child) {
            return lhs.child < rhs.child;
        }
        return lhs.parent < rhs.parent;
    });

    const auto noParent = static_cast<std::size_t>(-1);
    std::vector<std::size_t> parentOf(attrCount, noParent);
    for (const auto& edge : edges) {
        if (parentOf[edge.child] != noParent ||
            WouldCreateIntraFieldCycle(edge.parent, edge.child, parentOf, noParent)) {
            continue;
        }
        parentOf[edge.child] = edge.parent;
    }

    for (std::size_t child = 0; child < attrCount; ++child) {
        const auto parent = parentOf[child];
        if (parent == noParent) {
            continue;
        }
        if (metaIndices[parent] > std::numeric_limits<std::uint16_t>::max()) {
            return validation::AssignError(error, "attribute reference parent meta index exceeds uint16 range");
        }
        auto& entry = schedule.entries[child];
        entry.hasIntraParent = true;
        entry.parentMetaIndex = static_cast<std::uint16_t>(metaIndices[parent]);
        entry.parentMeta = metas[parent];
        entry.parentSource = sources[parent];
    }
    if (!BuildIntraFieldRecordOrder(parentOf, noParent, schedule.topologyOrder, error)) {
        return false;
    }
    return true;
    });
    if (!selected) { return false; }
    edges = {};
    edgeOwner.reset();
    schedule.initialized = true;
    return true;
}

} // namespace datacodec

#endif
