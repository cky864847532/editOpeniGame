#include "DataCodec/Storage/ByteIO/FileByteRangeIO.h"
#include "DataCodec/Storage/ByteIO/FileByteRangeIOTestHooks.h"
#include "DataCodec/Validation/Common/DataCodecValidation.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace datacodec {
namespace {

constexpr std::size_t kMappingWindowBytes = 4u * 1024u * 1024u;

bool FileError(std::string* error, const char* operation) {
    if (error == nullptr) { return false; }
#ifdef _WIN32
    const std::error_code code(static_cast<int>(GetLastError()), std::system_category());
#else
    const std::error_code code(errno, std::generic_category());
#endif
    return validation::AssignError(error, std::string(operation) + ": " + code.message());
}

class NativeFile final {
public:
    ~NativeFile() { Close(nullptr); }
    NativeFile() = default;
    NativeFile(const NativeFile&) = delete;
    NativeFile& operator=(const NativeFile&) = delete;

    bool IsOpen() const noexcept {
#ifdef _WIN32
        return m_handle != INVALID_HANDLE_VALUE;
#else
        return m_handle >= 0;
#endif
    }

    bool Open(const std::filesystem::path& path, bool write, std::string* error) {
#ifdef _WIN32
        m_handle = CreateFileW(path.c_str(), write ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ,
            FILE_SHARE_READ, nullptr, write ? CREATE_NEW : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
#else
        m_handle = ::open(path.c_str(), write ? O_RDWR | O_CREAT | O_EXCL : O_RDONLY, 0600);
#endif
        return IsOpen() || FileError(error, "open file");
    }

    bool Size(std::uint64_t& value, std::string* error) const {
#ifdef _WIN32
        LARGE_INTEGER bytes{};
        if (!GetFileSizeEx(m_handle, &bytes) || bytes.QuadPart < 0) { return FileError(error, "query file size"); }
        value = static_cast<std::uint64_t>(bytes.QuadPart);
#else
        struct stat info{};
        if (::fstat(m_handle, &info) != 0 || info.st_size < 0) { return FileError(error, "query file size"); }
        value = static_cast<std::uint64_t>(info.st_size);
#endif
        return true;
    }

    bool Resize(std::uint64_t size, std::string* error) {
        if (fileiotest::ShouldFail(fileiotest::FailurePoint::Resize)) {
            return validation::AssignError(error, "injected file resize failure");
        }
        if (size > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return validation::AssignError(error, "file size exceeds platform offset range");
        }
#ifdef _WIN32
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(size);
        return (SetFilePointerEx(m_handle, position, nullptr, FILE_BEGIN) && SetEndOfFile(m_handle)) ||
            FileError(error, "resize file");
#else
        if (size > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
            return validation::AssignError(error, "file size exceeds platform offset range");
        }
        return ::ftruncate(m_handle, static_cast<off_t>(size)) == 0 || FileError(error, "resize file");
#endif
    }

    bool Transfer(std::uint64_t offset, void* data, std::size_t bytes, bool write, std::string* error) {
        auto* cursor = static_cast<std::uint8_t*>(data);
        while (bytes != 0u) {
            const auto count = std::min(bytes, kMappingWindowBytes);
#if defined(__EMSCRIPTEN__)
            // 浏览器虚拟文件系统使用范围 IO，浏览器 File 输入由平台 reader 分段读取
            const auto done = write ? ::pwrite(m_handle, cursor, count, static_cast<off_t>(offset))
                                    : ::pread(m_handle, cursor, count, static_cast<off_t>(offset));
            if (done < 0 && errno == EINTR) { continue; }
            if (done <= 0) { return FileError(error, "transfer file range"); }
            const auto transferred = static_cast<std::size_t>(done);
#else
#ifdef _WIN32
            SYSTEM_INFO info{};
            GetSystemInfo(&info);
            const auto alignment = static_cast<std::uint64_t>(info.dwAllocationGranularity);
#else
            const auto pageSize = ::sysconf(_SC_PAGESIZE);
            if (pageSize <= 0) { return FileError(error, "query mapping alignment"); }
            const auto alignment = static_cast<std::uint64_t>(pageSize);
#endif
            const auto aligned = offset - offset % alignment;
            const auto prefix = static_cast<std::size_t>(offset - aligned);
            const auto mappedBytes = prefix + count;
#ifdef _WIN32
            const auto mapping = CreateFileMappingW(m_handle, nullptr, write ? PAGE_READWRITE : PAGE_READONLY, 0, 0, nullptr);
            if (mapping == nullptr) { return FileError(error, "create file mapping"); }
            if (fileiotest::ShouldFail(fileiotest::FailurePoint::Map)) {
                CloseHandle(mapping);
                return validation::AssignError(error, "injected file mapping failure");
            }
            void* view = MapViewOfFile(mapping, write ? FILE_MAP_WRITE : FILE_MAP_READ,
                static_cast<DWORD>(aligned >> 32u), static_cast<DWORD>(aligned), mappedBytes);
            if (view == nullptr) {
                const auto code = GetLastError();
                CloseHandle(mapping);
                SetLastError(code);
                return FileError(error, "map file range");
            }
#else
            if (fileiotest::ShouldFail(fileiotest::FailurePoint::Map)) {
                return validation::AssignError(error, "injected file mapping failure");
            }
            void* view = ::mmap(nullptr, mappedBytes, write ? PROT_READ | PROT_WRITE : PROT_READ,
                MAP_SHARED, m_handle, static_cast<off_t>(aligned));
            if (view == MAP_FAILED) { return FileError(error, "map file range"); }
#endif
            auto* mapped = static_cast<std::uint8_t*>(view) + prefix;
            if (write) { std::memcpy(mapped, cursor, count); }
            else { std::memcpy(cursor, mapped, count); }
            const char* failedOperation = nullptr;
            const bool injectFlushFailure = write && fileiotest::ShouldFail(fileiotest::FailurePoint::FlushMappedRange);
#ifdef _WIN32
            DWORD failureCode = ERROR_SUCCESS;
            if (injectFlushFailure || (write && !FlushViewOfFile(view, mappedBytes))) {
                failureCode = injectFlushFailure ? ERROR_WRITE_FAULT : GetLastError(); failedOperation = "flush mapped range";
            }
            if (!UnmapViewOfFile(view) && failedOperation == nullptr) {
                failureCode = GetLastError(); failedOperation = "unmap file range";
            }
            if (!CloseHandle(mapping) && failedOperation == nullptr) {
                failureCode = GetLastError(); failedOperation = "close file mapping";
            }
            if (failedOperation != nullptr) { SetLastError(failureCode); }
#else
            int failureCode = 0;
            if (injectFlushFailure || (write && ::msync(view, mappedBytes, MS_SYNC) != 0)) {
                failureCode = injectFlushFailure ? EIO : errno; failedOperation = "flush mapped range";
            }
            if (::munmap(view, mappedBytes) != 0 && failedOperation == nullptr) {
                failureCode = errno; failedOperation = "unmap file range";
            }
            if (failedOperation != nullptr) { errno = failureCode; }
#endif
            if (failedOperation != nullptr) { return FileError(error, failedOperation); }
            const auto transferred = count;
#endif
            cursor += transferred;
            offset += transferred;
            bytes -= transferred;
        }
        return true;
    }

    bool Flush(std::string* error) {
        if (fileiotest::ShouldFail(fileiotest::FailurePoint::FlushFile)) {
            return validation::AssignError(error, "injected file flush failure");
        }
#ifdef _WIN32
        return FlushFileBuffers(m_handle) || FileError(error, "flush file");
#else
        return ::fsync(m_handle) == 0 || FileError(error, "flush file");
#endif
    }

    bool ZeroRange(std::uint64_t offset, std::uint64_t bytes, std::string* error) {
        static const std::array<std::uint8_t, 64u * 1024u> zeros{};
        while (bytes != 0u) {
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(bytes, zeros.size()));
            if (!Transfer(offset, const_cast<std::uint8_t*>(zeros.data()), count, true, error)) { return false; }
            offset += count;
            bytes -= count;
        }
        return true;
    }

    bool Close(std::string* error) {
        if (!IsOpen()) { return true; }
#ifdef _WIN32
        const auto handle = std::exchange(m_handle, INVALID_HANDLE_VALUE);
        return CloseHandle(handle) || FileError(error, "close file");
#else
        const auto handle = std::exchange(m_handle, -1);
        return ::close(handle) == 0 || FileError(error, "close file");
#endif
    }

private:
#ifdef _WIN32
    HANDLE m_handle{INVALID_HANDLE_VALUE};
#else
    int m_handle{-1};
#endif
};

} // 匿名命名空间

struct FileByteRangeReader::Impl {
    explicit Impl(const std::filesystem::path& path) {
        if (file.Open(path, false, &openError)) { file.Size(size, &openError); }
    }
    NativeFile file;
    std::uint64_t size{0u};
    std::string openError;
    std::mutex mutex;
};

FileByteRangeReader::FileByteRangeReader(std::filesystem::path path) : m_impl(std::make_unique<Impl>(path)) {}
FileByteRangeReader::~FileByteRangeReader() = default;
std::uint64_t FileByteRangeReader::ByteSize() const noexcept { return m_impl->size; }
bool FileByteRangeReader::ReadAt(std::uint64_t offset, std::span<std::uint8_t> output, std::string* error) {
    std::lock_guard lock(m_impl->mutex);
    if (!m_impl->openError.empty()) { return validation::AssignError(error, m_impl->openError); }
    if (offset > m_impl->size || output.size() > m_impl->size - offset) {
        return validation::AssignError(error, "file reader range is outside the file");
    }
    return m_impl->file.Transfer(offset, output.data(), output.size(), false, error);
}

struct FileByteRangeOutput::Impl {
    explicit Impl(std::filesystem::path destination) : path(std::move(destination)) {}
    ~Impl() {
        file.Close(nullptr);
        if (!temporary.empty()) { std::error_code ignored; std::filesystem::remove(temporary, ignored); }
    }
    bool Open(std::string* error) {
        if (file.IsOpen()) { return true; }
        std::error_code code;
        if (!path.parent_path().empty()) { std::filesystem::create_directories(path.parent_path(), code); }
        if (code) { return validation::AssignError(error, "create output directory: " + code.message()); }
        static std::atomic_uint64_t next{0u};
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        auto candidate = path;
        candidate += ".datacodec-" + std::to_string(nonce) + "-" + std::to_string(next.fetch_add(1u)) + ".tmp";
        if (!file.Open(candidate, true, error)) { return false; }
        temporary = std::move(candidate);
        return true;
    }
    std::filesystem::path path, temporary;
    NativeFile file;
    std::uint64_t size{0u};
    bool failed{false}, finalized{false};
    std::mutex mutex;
};

FileByteRangeOutput::FileByteRangeOutput(std::filesystem::path path) : m_impl(std::make_unique<Impl>(std::move(path))) {}
FileByteRangeOutput::~FileByteRangeOutput() = default;
std::uint64_t FileByteRangeOutput::LogicalSize() const noexcept { return m_impl->size; }
bool FileByteRangeOutput::WriteAt(std::uint64_t offset, std::span<const std::uint8_t> bytes, std::string* error) {
    std::lock_guard lock(m_impl->mutex);
    auto& state = *m_impl;
    if (state.failed || state.finalized) { return validation::AssignError(error, "file output is not writable"); }
    std::uint64_t end = 0u;
    if (!validation::CheckedAddU64(offset, bytes.size(), end, "file output range", error) || !state.Open(error) ||
        (end > state.size && !state.file.Resize(end, error)) ||
        (offset > state.size && !state.file.ZeroRange(state.size, offset - state.size, error)) ||
        !state.file.Transfer(offset, const_cast<std::uint8_t*>(bytes.data()), bytes.size(), true, error)) {
        state.failed = true;
        return false;
    }
    state.size = std::max(state.size, end);
    return true;
}

bool FileByteRangeOutput::Finalize(std::uint64_t logicalSize, std::string* error) {
    std::lock_guard lock(m_impl->mutex);
    auto& state = *m_impl;
    if (state.failed || state.finalized) { return validation::AssignError(error, "file output cannot be finalized again"); }
    if (!state.Open(error) || !state.file.Resize(logicalSize, error) ||
        (logicalSize > state.size && !state.file.ZeroRange(state.size, logicalSize - state.size, error)) ||
        !state.file.Flush(error) || !state.file.Close(error)) {
        state.failed = true;
        state.file.Close(nullptr);
        return false;
    }
#ifdef _WIN32
    if (!MoveFileExW(state.temporary.c_str(), state.path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        state.failed = true;
        return FileError(error, "publish file output");
    }
#else
    std::error_code code;
    std::filesystem::rename(state.temporary, state.path, code);
    if (code) { state.failed = true; return validation::AssignError(error, "publish file output: " + code.message()); }
#endif
    state.size = logicalSize;
    state.temporary.clear();
    state.finalized = true;
    return true;
}

} // 命名空间 datacodec
