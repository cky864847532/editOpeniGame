#include "DataCodec/Platform/Wasm/WasmBrowserFileByteRangeReader.h"

#include "DataCodec/Validation/Common/DataCodecValidation.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten/em_js.h>
#endif

namespace datacodec::wasm {

#if defined(__EMSCRIPTEN__)
EM_ASYNC_JS(int, datacodec_wasm_read_browser_file_range,
            (std::uint32_t fileId, std::uint64_t offset, std::uint8_t* output, std::size_t byteCount), {
    try {
        const registry = Module.igameBrowserFiles;
        const file = registry && registry.get(fileId);
        if (!file) {
            Module.igameBrowserFileLastError = `browser file ${fileId} is unavailable`;
            return -1;
        }
        const start = Number(offset);
        const count = Number(byteCount);
        const destination = Number(output);
        if (!Number.isSafeInteger(start) || start < 0 ||
            !Number.isSafeInteger(count) || count < 0 || count > 1048576 ||
            !Number.isSafeInteger(destination) || destination < 0 ||
            !Number.isSafeInteger(start + count) || start + count > file.size) {
            Module.igameBrowserFileLastError =
                `invalid browser file range offset=${start} bytes=${count} size=${file.size}`;
            return -2;
        }
        let bytes = new Uint8Array(await file.slice(start, start + count).arrayBuffer());
        if (bytes.byteLength !== count) {
            Module.igameBrowserFileLastError =
                `browser file range returned ${bytes.byteLength} bytes, expected ${count}`;
            return -3;
        }
        if (destination > HEAPU8.length || count > HEAPU8.length - destination) {
            Module.igameBrowserFileLastError = 'browser file destination exceeds linear memory';
            return -2;
        }
        HEAPU8.set(bytes, destination);
        bytes = null;
        if (typeof Module.igameBrowserFileReadObserver === 'function') {
            try { Module.igameBrowserFileReadObserver(fileId, start, count); }
            catch (error) {
                Module.igameBrowserFileDiagnosticFailures = (Module.igameBrowserFileDiagnosticFailures || 0) + 1;
            }
        }
        Module.igameBrowserFileLastError = String();
        return 1;
    } catch (error) {
        Module.igameBrowserFileLastError = error && error.message
            ? error.message
            : String(error);
        return -4;
    }
});

EM_JS(void, datacodec_wasm_release_browser_file, (std::uint32_t fileId), {
    if (Module.igameBrowserFiles) Module.igameBrowserFiles.delete(fileId);
    if (Module.igameBrowserFileStats) Module.igameBrowserFileStats.delete(fileId);
});
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
        return validation::AssignError(error, "browser file read range is invalid");
    }
    if (output.empty()) { return true; }
#if defined(__EMSCRIPTEN__)
    for (std::size_t consumed = 0u; consumed < output.size();) {
        const auto count = std::min(output.size() - consumed, kIoWindowBytes);
        const auto status = datacodec_wasm_read_browser_file_range(
            m_fileId, offset + consumed, output.data() + consumed, count);
        if (status != 1) {
            return validation::AssignError(error,
                "browser file range read failed with status " + std::to_string(status));
        }
        consumed += count;
    }
    return true;
#else
    return validation::AssignError(error, "browser file reader requires Emscripten");
#endif
}

std::uint32_t WasmBrowserFileByteRangeReader::FileId() const noexcept {
    return m_fileId;
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
        datacodec_wasm_release_browser_file(fileId);
    }
#else
    (void)fileId;
#endif
}

} // namespace datacodec::wasm
