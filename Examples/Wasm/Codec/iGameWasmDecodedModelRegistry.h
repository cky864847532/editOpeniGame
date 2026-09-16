#ifndef iGameWasmDecodedModelRegistry_h
#define iGameWasmDecodedModelRegistry_h

#include "DataCodec/Filter/Adapter/iGameDataCodecDataObjectBridge.h"
#include "DataCodec/API/Adapter/DecodeCacheIdentity.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

IGAME_NAMESPACE_BEGIN

struct iGameWasmDecodedModelEntry {
    std::shared_ptr<DataCodecDataObjectDecodeSession> codec;
    ::datacodec::DecodeSourceIdentity sourceIdentity;
    std::string ownedInputPath;
    std::uint32_t browserFileId{0u};
    ::datacodec::EncodedInput deferredInput;
};

class iGameWasmDecodedModelRegistry final {
public:
    ~iGameWasmDecodedModelRegistry();

    [[nodiscard]] bool Contains(std::uint32_t modelId) const;
    [[nodiscard]] int FindBySource(
        const ::datacodec::DecodeSourceIdentity& sourceIdentity, bool requireRawData = false) const;
    [[nodiscard]] iGameWasmDecodedModelEntry* Find(std::uint32_t modelId);
    [[nodiscard]] const iGameWasmDecodedModelEntry* Find(std::uint32_t modelId) const;
    [[nodiscard]] DataObject::Pointer RestoreRawData(std::uint32_t modelId,
        const ::datacodec::CodecResourceParams& resources, std::string* error = nullptr,
        std::optional<::datacodec::CodecFailureRecord>* failure = nullptr);
    void Store(std::uint32_t modelId, iGameWasmDecodedModelEntry entry);
    void Erase(std::uint32_t modelId);
    void Clear();
    [[nodiscard]] std::vector<std::uint32_t> ModelIds() const;

private:
    static void ReleaseInput(iGameWasmDecodedModelEntry& entry);

    std::map<std::uint32_t, iGameWasmDecodedModelEntry> m_entries;
    std::uint64_t m_revision{0u};
};

IGAME_NAMESPACE_END

#endif
