#ifndef DATACODEC_RUNTIME_CACHE_DECODECACHE_DECODEDSTORAGESIZE_H
#define DATACODEC_RUNTIME_CACHE_DECODECACHE_DECODEDSTORAGESIZE_H

#include "DataCodec/API/Params/CodecStorageParams.h"
#include "DataCodec/Common/Views/ArrayViews.h"
#include "DataCodec/Validation/Common/DataCodecValidation.h"

namespace datacodec {

// 预检和真实分配共用确定容量公式，所有乘加均检查溢出
inline bool CalculateGeometryCacheBytes(const std::size_t count, const std::size_t dimension,
    DataType type, std::uint64_t& bytes, std::string* error = nullptr) {
    std::uint64_t values = 0u;
    return validation::CheckedMulU64(count, dimension, values, "decoded geometry value count", error) &&
        validation::CheckedMulU64(values, ScalarTypeSize(ToScalarType(type)), bytes, "decoded geometry cache bytes", error);
}

inline bool CalculateDecodedNumericStorageBytes(const NumericArrayStorageParams& meta,
    std::uint64_t& bytes, std::string* error = nullptr) {
    std::uint64_t tupleBytes = 0u;
    if (meta.dimension < 0 || NumericArrayValueSize(meta) == 0u) {
        return validation::AssignError(error, "decoded numeric storage shape is invalid");
    }
    return validation::CheckedMulU64(static_cast<std::uint64_t>(meta.dimension),
               NumericArrayValueSize(meta), tupleBytes, "decoded numeric tuple bytes", error) &&
        validation::CheckedMulU64(meta.elementCount, tupleBytes, bytes, "decoded numeric storage bytes", error);
}

struct DecodedConnectivityStorageSize {
    std::size_t connectivity{}, offsets{}, cellTypes{}, polynomialOrders{};
};

inline bool CalculateDecodedConnectivityStorageSize(const std::size_t cells, const std::size_t indices,
    const bool offsets, const bool types, const bool orders, DecodedConnectivityStorageSize& size,
    std::string* error = nullptr) {
    size = {};
    std::size_t offsetCount = 0u;
    return (!offsets || validation::CheckedAddSizeT(cells, 1u, offsetCount, "decoded topology offset count", error)) &&
        validation::CheckedMulSizeT(indices, sizeof(IndexType), size.connectivity, "decoded topology connectivity bytes", error) &&
        validation::CheckedMulSizeT(offsetCount, sizeof(IndexType), size.offsets, "decoded topology offset bytes", error) &&
        (!types || validation::CheckedMulSizeT(cells, sizeof(IndexType), size.cellTypes, "decoded topology cell type bytes", error)) &&
        (!orders || validation::CheckedMulSizeT(cells, sizeof(std::uint16_t), size.polynomialOrders,
            "decoded topology polynomial order bytes", error));
}

}
#endif
