#include "DataCodec/Platform/Wasm/WasmBrowserFileByteRangeReader.h"

#include "DataCodec/Validation/Common/DataCodecValidation.h"

#include <array>
#include <limits>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#endif

namespace datacodec::wasm {

#if defined(__EMSCRIPTEN__)
namespace {

int ReadBrowserFileRange(std::uint32_t fileId, std::uint64_t offset,
    std::uint8_t* output, std::size_t byteCount, std::span<char> errorText) {
    alignas(4) std::int32_t status = 0;
    // 主线程持有 File 对象，异步读取结束前请求状态和目标缓冲保持存活
    MAIN_THREAD_ASYNC_EM_ASM({
        const fileId = Number($0);
        const start = Number($1);
        const destination = Number($2);
        const count = Number($3);
        const statusIndex = Number($4) / 4;
        const errorAddress = Number($5);
        const errorCapacity = Number($6);
        const finish = (status, detail) => {
            if (Module.dataCodecBrowserFiles) Module.dataCodecBrowserFiles.recordError(detail);
            stringToUTF8(detail, errorAddress, errorCapacity);
            Atomics.store(HEAP32, statusIndex, status);
            Atomics.notify(HEAP32, statusIndex);
        };
        (async () => {
            try {
                const registry = Module.dataCodecBrowserFiles;
                if (!registry) { finish(-1, 'DataCodec browser file bridge is unavailable'); return; }
                if (!Number.isSafeInteger(destination) || destination < 0) {
                    finish(-2, 'invalid browser file destination');
                    return;
                }
                let bytes = await registry.read(fileId, start, count);
                if (destination > HEAPU8.length || count > HEAPU8.length - destination) {
                    finish(-2, 'browser file destination exceeds linear memory');
                    return;
                }
                HEAPU8.set(bytes, destination);
                bytes = null;
                finish(1, String());
            } catch (error) {
                finish(-4, error && error.message ? error.message : String(error));
            }
        })();
    }, fileId, offset, output, byteCount, &status, errorText.data(), errorText.size());
    while (__atomic_load_n(&status, __ATOMIC_ACQUIRE) == 0) {
        if (emscripten_is_main_browser_thread()) {
            emscripten_sleep(0);
        } else {
            emscripten_futex_wait(&status, 0u, std::numeric_limits<double>::infinity());
        }
    }
    return __atomic_load_n(&status, __ATOMIC_ACQUIRE);
}

}
#endif

WasmBrowserFileByteRangeReader::WasmBrowserFileByteRangeReader(
    const std::uint32_t fileId, const std::uint64_t byteSize)
    : m_fileId(fileId), m_byteSize(byteSize) {}

std::uint64_t WasmBrowserFileByteRangeReader::ByteSize() const noexcept {
    return m_byteSize;
}

ByteRangePrefetchResult WasmBrowserFileByteRangeReader::PrefetchRange(
    const std::uint64_t, const std::uint64_t) const {
    return {.status = ByteRangePrefetchStatus::Unavailable};
}

bool WasmBrowserFileByteRangeReader::ReadAt(
    const std::uint64_t offset,
    const std::span<std::uint8_t> output,
    std::string* error) {
    if (m_fileId == 0u || offset > m_byteSize || output.size() > m_byteSize - offset) {
        return FailRead("browser file read range is invalid", error);
    }
    if (output.empty()) { return true; }
#if defined(__EMSCRIPTEN__)
    for (std::size_t consumed = 0u; consumed < output.size();) {
        const auto count = std::min(output.size() - consumed, kIoWindowBytes);
        std::array<char, 512u> detail{};
        const auto status = ReadBrowserFileRange(
            m_fileId, offset + consumed, output.data() + consumed, count, detail);
        if (status != 1) {
            return FailRead("browser file range read failed: fileId=" + std::to_string(m_fileId) +
                " offset=" + std::to_string(offset + consumed) + " bytes=" + std::to_string(count) +
                " status=" + std::to_string(status) + " detail=" + detail.data(), error);
        }
        consumed += count;
    }
    return true;
#else
    return FailRead("browser file reader requires Emscripten", error);
#endif
}

std::uint32_t WasmBrowserFileByteRangeReader::FileId() const noexcept {
    return m_fileId;
}

std::string WasmBrowserFileByteRangeReader::LastReadError() const {
    std::lock_guard lock(m_errorMutex);
    return m_lastReadError;
}

bool WasmBrowserFileByteRangeReader::FailRead(std::string message, std::string* error) {
    {
        std::lock_guard lock(m_errorMutex);
        if (m_lastReadError.empty()) { m_lastReadError = message; }
    }
    return validation::AssignError(error, std::move(message));
}

std::shared_ptr<IByteRangeReader> CreateWasmBrowserFileByteRangeReader(
    const std::uint32_t fileId,
    const std::uint64_t byteSize,
    std::string* error) {
#if defined(__EMSCRIPTEN__)
    if (fileId == 0u) {
        validation::AssignError(error, "browser file id is invalid");
        return nullptr;
    }
    if (byteSize == 0u) {
        validation::AssignError(error, "browser file is empty");
        return nullptr;
    }
    return std::make_shared<WasmBrowserFileByteRangeReader>(fileId, byteSize);
#else
    (void)fileId;
    (void)byteSize;
    validation::AssignError(error, "browser file reader requires Emscripten");
    return nullptr;
#endif
}

void ReleaseWasmBrowserFile(const std::uint32_t fileId) noexcept {
#if defined(__EMSCRIPTEN__)
    if (fileId != 0u) {
        MAIN_THREAD_ASYNC_EM_ASM({
            const fileId = Number($0);
            if (Module.dataCodecBrowserFiles) Module.dataCodecBrowserFiles.release(fileId);
        }, fileId);
    }
#else
    (void)fileId;
#endif
}

} // namespace datacodec::wasm
