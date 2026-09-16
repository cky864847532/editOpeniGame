#ifndef iGameWasmDataCodecBridge_h
#define iGameWasmDataCodecBridge_h

#include "DataCodec/API/Adapter/IRunRecordSink.h"
#include "DataCodec/API/Adapter/EncodedInputTypes.h"
#include "DataCodec/Filter/Adapter/iGameDataCodecDataObjectBridge.h"

#include "DataCodec/API/Adapter/DecodedFrameTypes.h"

#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string>

IGAME_NAMESPACE_BEGIN

struct iGameWasmDataCodecDecodeRequest {
    ::datacodec::EncodedInput input;
    ::datacodec::DecodeSourceIdentity sourceIdentity;
    bool enableReuseCache{true};
    std::optional<bool> enableEncodedInputCache;
    ::datacodec::CodecResourceParams resources{.mode = ::datacodec::CodecResourceMode::Unlimited};
    std::shared_ptr<::datacodec::IRunRecordSink> runRecordSink;
};

struct iGameWasmDataCodecDecodeResult {
    bool success{false};
    DataObject::Pointer output;
    std::shared_ptr<DataCodecDataObjectDecodeSession> session;
    ::datacodec::DecodeSourceIdentity sourceIdentity;
    DataCodecDataObjectDecodeResult decodeResult;
    ::datacodec::EncodedInputCacheStats encodedInputCacheStatsBefore;
    ::datacodec::EncodedInputCacheStats encodedInputCacheStatsAfter;
    bool cacheIdentityAvailable{false};
    bool encodedInputCacheEnabled{false};
    bool diagnosticsIncomplete{false};
    std::string timingDetail;

    std::string error;
};

[[nodiscard]] std::future<void> SubmitiGameWasmDataCodecTask(
    std::function<void()> task);

[[nodiscard]] bool ResolveiGameWasmPackageSourceIdentity(
    const ::datacodec::EncodedInput& input,
    ::datacodec::DecodeSourceIdentity& sourceIdentity,
    std::string* error = nullptr);

[[nodiscard]] bool ResolveiGameWasmDataCodecFileSourceIdentity(
    const std::string& filePath,
    ::datacodec::DecodeSourceIdentity& sourceIdentity,
    std::string* error = nullptr);

[[nodiscard]] iGameWasmDataCodecDecodeResult DecodeiGameWasmDataCodec(
    iGameWasmDataCodecDecodeRequest request);

[[nodiscard]] iGameWasmDataCodecDecodeResult DecodeiGameWasmDataCodecFile(
    const std::string& filePath,
    bool enableReuseCache = true,
    std::optional<bool> enableEncodedInputCache = {},
    std::shared_ptr<::datacodec::IRunRecordSink> runRecordSink = {},
    ::datacodec::DecodeSourceIdentity sourceIdentity = {},
    ::datacodec::CodecResourceParams resources = {.mode = ::datacodec::CodecResourceMode::Unlimited});

[[nodiscard]] iGameWasmDataCodecDecodeResult DecodeiGameWasmDataCodecMemory(
    std::shared_ptr<const void> inputOwner,
    std::span<const std::uint8_t> bytes,
    bool enableReuseCache = true,
    std::shared_ptr<::datacodec::IRunRecordSink> runRecordSink = {},
    ::datacodec::CodecResourceParams resources = {.mode = ::datacodec::CodecResourceMode::Unlimited});

[[nodiscard]] iGameWasmDataCodecDecodeResult DecodeiGameWasmBrowserFile(
    std::uint32_t browserFileId,
    std::uint64_t browserFileSize,
    bool enableReuseCache = true,
    std::optional<bool> enableEncodedInputCache = {},
    std::shared_ptr<::datacodec::IRunRecordSink> runRecordSink = {},
    ::datacodec::CodecResourceParams resources = {.mode = ::datacodec::CodecResourceMode::Unlimited});

IGAME_NAMESPACE_END

#endif
