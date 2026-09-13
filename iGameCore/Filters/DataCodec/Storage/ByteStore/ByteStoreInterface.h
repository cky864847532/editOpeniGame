#ifndef DATACODEC_STORAGE_BYTESTORE_BYTESTOREINTERFACE_H
#define DATACODEC_STORAGE_BYTESTORE_BYTESTOREINTERFACE_H

#include "DataCodec/Storage/ByteIO/ByteSource.h"

namespace datacodec::bytestore {

// 存储接口不依赖预算、线程池和具体存储实现
class IAppendableByteStore : public IByteSource {
public:
    virtual bool AppendBytes(std::span<const std::uint8_t> bytes, std::string* error = nullptr) = 0;
    virtual bool Seal(std::string* error = nullptr) = 0;

    bool AppendBytes(std::vector<std::uint8_t> bytes, std::string* error = nullptr) {
        const auto ok = AppendBytes(std::span<const std::uint8_t>(bytes.data(), bytes.size()), error);
        std::vector<std::uint8_t>().swap(bytes);
        return ok;
    }
};

// 接入原生解码数组时，shared_ptr 持有完整存储及其宿主 owner
// 完成 ResizeBytes 后允许并发写入互不重叠区间，写入期间不得重定位底层数组
// Seal 在全部写入完成后由串行 driver 调用，之后必须支持范围读取
// 普通具体存储的动态扩容仍由其创建者串行安排
class IRandomAccessByteStore : public IAppendableByteStore {
public:
    virtual bool ResizeBytes(std::uint64_t byteSize, std::string* error = nullptr) = 0;
    virtual bool WriteBytesAt(std::uint64_t offset, std::span<const std::uint8_t> bytes,
                              std::string* error = nullptr) = 0;
};

} // 命名空间 datacodec::bytestore

#endif
