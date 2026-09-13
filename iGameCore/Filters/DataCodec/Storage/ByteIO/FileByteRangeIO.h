#ifndef DATACODEC_STORAGE_BYTEIO_FILEBYTERANGEIO_H
#define DATACODEC_STORAGE_BYTEIO_FILEBYTERANGEIO_H

#include "DataCodec/Storage/ByteIO/ByteRange.h"
#include "DataCodec/Validation/Common/DataCodecValidation.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <mutex>

namespace datacodec {

// 文件输入只读取当前请求范围，输入页面不随整个 reader 累积为映射工作集
class FileByteRangeReader final : public IByteRangeReader {
public:
    explicit FileByteRangeReader(std::filesystem::path path)
        : m_input(path, std::ios::binary) {
        std::error_code error;
        m_byteSize = std::filesystem::file_size(path, error);
        if (error) { m_byteSize = 0u; }
    }

    [[nodiscard]] std::uint64_t ByteSize() const noexcept override { return m_byteSize; }

    bool ReadAt(const std::uint64_t offset, const std::span<std::uint8_t> output,
        std::string* error = nullptr) override {
        if (offset > m_byteSize || output.size() > m_byteSize - offset ||
            offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
            output.size() > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
            return ::datacodec::validation::AssignError(error, "file reader range is outside the file");
        }
        if (output.empty()) { return true; }
        std::lock_guard lock(m_mutex);
        if (!m_input.is_open()) {
            return ::datacodec::validation::AssignError(error, "failed to open file reader");
        }
        m_input.clear();
        m_input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        m_input.read(reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(output.size()));
        return (m_input && static_cast<std::size_t>(m_input.gcount()) == output.size()) ||
            ::datacodec::validation::AssignError(error, "failed to read file reader");
    }

private:
    std::ifstream m_input;
    std::uint64_t m_byteSize{0u};
    std::mutex m_mutex;
};

class FileByteRangeOutput final : public IByteRangeOutput {
public:
    explicit FileByteRangeOutput(std::filesystem::path path)
        : m_path(std::move(path)) {}

    bool WriteAt(
        const std::uint64_t offset,
        const std::span<const std::uint8_t> bytes,
        std::string* error = nullptr) override {
        if (bytes.empty()) {
            m_logicalSize = std::max(m_logicalSize, offset);
            return true;
        }
        if (!EnsureOpen(error)) {
            return false;
        }
        m_stream.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!m_stream) {
            return ::datacodec::validation::AssignError(error, "failed to seek file output");
        }
        m_stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!m_stream) {
            return ::datacodec::validation::AssignError(error, "failed to write file output");
        }
        m_logicalSize = std::max(m_logicalSize, offset + static_cast<std::uint64_t>(bytes.size()));
        return true;
    }

    bool Finalize(const std::uint64_t logicalSize, std::string* error = nullptr) override {
        m_logicalSize = logicalSize;
        if (m_stream.is_open()) {
            m_stream.flush();
            if (!m_stream) {
                return ::datacodec::validation::AssignError(error, "failed to flush file output");
            }
            m_stream.close();
        } else if (!EnsureOpen(error)) {
            return false;
        } else {
            m_stream.close();
        }
        std::error_code resizeError;
        std::filesystem::resize_file(m_path, m_logicalSize, resizeError);
        if (resizeError) {
            return ::datacodec::validation::AssignError(error, "failed to finalize file output");
        }
        return true;
    }

    [[nodiscard]] std::uint64_t LogicalSize() const noexcept { return m_logicalSize; }

private:
    bool EnsureOpen(std::string* error) {
        if (m_stream.is_open()) {
            return true;
        }
        const auto directory = m_path.parent_path();
        if (!directory.empty()) {
            std::error_code createError;
            std::filesystem::create_directories(directory, createError);
            if (createError) {
                return ::datacodec::validation::AssignError(error, "failed to create file output directory");
            }
        }
        m_stream.open(m_path, std::ios::binary | std::ios::trunc | std::ios::in | std::ios::out);
        if (!m_stream.is_open()) {
            return ::datacodec::validation::AssignError(error, "failed to open file output");
        }
        return true;
    }

    std::filesystem::path m_path;
    std::fstream m_stream;
    std::uint64_t m_logicalSize{0u};
};

} // 命名空间 datacodec

#endif
