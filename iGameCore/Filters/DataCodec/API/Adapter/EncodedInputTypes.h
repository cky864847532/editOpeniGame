#ifndef DATACODEC_API_ADAPTER_ENCODEDINPUTTYPES_H
#define DATACODEC_API_ADAPTER_ENCODEDINPUTTYPES_H

#include "DataCodec/API/Adapter/CacheAccessResult.h"
#include "DataCodec/API/Adapter/DecodeCacheIdentity.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace datacodec {

class IByteRangeReader;
using EncodedInputBuffer = std::shared_ptr<IByteRangeReader>;
using EncodedInputCacheLookupResult = CacheLookupResult<EncodedInputBuffer>;

enum class EncodedInputAccessKind : std::uint8_t {
    UserRequest = 0u,
    Prefetch = 1u,
};

struct EncodedInputCacheStats {
    std::uint64_t lookups{0u};
    std::uint64_t hits{0u};
    std::uint64_t misses{0u};
    std::uint64_t lookupErrors{0u};
    std::uint64_t stores{0u};
    std::uint64_t storeRejections{0u};
    std::uint64_t storeErrors{0u};
    std::uint64_t evictions{0u};
    std::size_t residentInputs{0u};
};

}

#endif
