#ifndef DATACODEC_STORAGE_BYTEIO_FILEBYTERANGEIO_H
#define DATACODEC_STORAGE_BYTEIO_FILEBYTERANGEIO_H

#include "DataCodec/Storage/ByteIO/ByteRange.h"
#include <filesystem>
#include <memory>

namespace datacodec {

// 映射视图只在同步范围操作内存活，调用返回时已解除映射
class FileByteRangeReader final : public IByteRangeReader {
public:
    explicit FileByteRangeReader(std::filesystem::path path);
    ~FileByteRangeReader() override;
    [[nodiscard]] std::uint64_t ByteSize() const noexcept override;
    bool ReadAt(std::uint64_t offset, std::span<std::uint8_t> output, std::string* error = nullptr) override;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// 输出先写同目录临时文件，完成刷新及关闭后原子替换目标
class FileByteRangeOutput final : public IByteRangeOutput {
public:
    explicit FileByteRangeOutput(std::filesystem::path path);
    ~FileByteRangeOutput() override;
    bool WriteAt(std::uint64_t offset, std::span<const std::uint8_t> bytes, std::string* error = nullptr) override;
    bool Finalize(std::uint64_t logicalSize, std::string* error = nullptr) override;
    [[nodiscard]] std::uint64_t LogicalSize() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // 命名空间 datacodec
#endif
