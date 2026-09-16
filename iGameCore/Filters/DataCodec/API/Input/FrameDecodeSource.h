#ifndef DATACODEC_API_INPUT_FRAMEDECODESOURCE_H
#define DATACODEC_API_INPUT_FRAMEDECODESOURCE_H

#include "DataCodec/API/Input/EncodedInput.h"
#include "DataCodec/API/Adapter/DecodeCacheIdentity.h"


#include <cstdint>
#include <memory>

namespace datacodec {

// 描述已发现的一帧包及其稳定缓存身份
struct FrameDecodeSource {
    std::uint32_t frameIndex{0u};
    float timeValue{0.0f};
    EncodedInput input;
    DecodeSourceIdentity sourceIdentity;

};

} // namespace datacodec

#endif
