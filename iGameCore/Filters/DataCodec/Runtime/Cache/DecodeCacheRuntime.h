#ifndef DATACODEC_RUNTIME_CACHE_DECODECACHERUNTIME_H
#define DATACODEC_RUNTIME_CACHE_DECODECACHERUNTIME_H

#include "DataCodec/Runtime/Cache/DecodeReferenceCache.h"
#include "DataCodec/Runtime/Cache/DecodedFrameLruCache.h"
#include "DataCodec/Runtime/Cache/EncodedInputCacheLoader.h"
#include "DataCodec/Runtime/Cache/EncodedInputLruCache.h"

#include <memory>

namespace datacodec {

class DecodeCacheRuntime final {
public:
    explicit DecodeCacheRuntime(DataCodecExecutionResources& run)
        : m_referenceCache(std::make_shared<DecodeReferenceCache>(&run)),
          m_defaultFrameCache(std::make_shared<DecodedFrameLruCache>(&run)),
          m_defaultEncodedInputCache(std::make_shared<EncodedInputLruCache>(&run)) {
        m_defaultFrameCache->Configure(2u);
        m_defaultEncodedInputCache->Configure(1u);
    }

    // 顺序逐项解除可选引用，实际容量由最后 owner 的 lease 归还
    bool TrimOne() {
        return m_defaultEncodedInputCache->TrimOne() ||
            m_defaultFrameCache->TrimOne() || m_referenceCache->TrimOne();
    }

    void TrimAll() { while (TrimOne()) {} }

    [[nodiscard]] std::shared_ptr<DecodeReferenceCache> ReferenceCache() const noexcept {
        return m_referenceCache;
    }

    [[nodiscard]] std::shared_ptr<DecodedFrameLruCache> DefaultFrameCache() const noexcept {
        return m_defaultFrameCache;
    }

    void SetDefaultDecodedFrameCacheEnabled(const bool enabled) {
        m_defaultFrameCache->SetEnabled(enabled);
    }

    [[nodiscard]] bool DefaultDecodedFrameCacheEnabled() const {
        return m_defaultFrameCache->IsEnabled();
    }

    [[nodiscard]] std::shared_ptr<EncodedInputLruCache> DefaultEncodedInputCache() const noexcept {
        return m_defaultEncodedInputCache;
    }

    [[nodiscard]] EncodedInputCacheLoader& EncodedInputLoader() noexcept {
        return m_encodedInputLoader;
    }

private:
    std::shared_ptr<DecodeReferenceCache> m_referenceCache;
    std::shared_ptr<DecodedFrameLruCache> m_defaultFrameCache;
    std::shared_ptr<EncodedInputLruCache> m_defaultEncodedInputCache;
    EncodedInputCacheLoader m_encodedInputLoader;
};

} // namespace datacodec

#endif
