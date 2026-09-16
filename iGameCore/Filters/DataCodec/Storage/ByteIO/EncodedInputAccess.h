#ifndef DATACODEC_STORAGE_BYTEIO_ENCODEDINPUTACCESS_H
#define DATACODEC_STORAGE_BYTEIO_ENCODEDINPUTACCESS_H

#include "DataCodec/API/Input/EncodedInput.h"
#include "DataCodec/Storage/ByteIO/ByteRange.h"

namespace datacodec {

// 已打开的范围源只在核心会话之间传递，不属于调用方输入合同
class EncodedInputAccess final {
public:
    static std::shared_ptr<IByteRangeReader> Open(const EncodedInput& input);
    static EncodedInput Retain(std::shared_ptr<IByteRangeReader> reader);
};

} // 命名空间 datacodec
#endif
