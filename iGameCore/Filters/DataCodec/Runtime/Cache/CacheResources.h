#ifndef DATACODEC_RUNTIME_CACHE_CACHERESOURCES_H
#define DATACODEC_RUNTIME_CACHE_CACHERESOURCES_H

#include "DataCodec/Common/DataCodecTypes.h"
#include "DataCodec/Runtime/Execution/DataCodecExecutionResources.h"
#include "DataCodec/Storage/ByteIO/ScratchByteBuffer.h"
#include "DataCodec/Storage/ByteIO/ByteBudget.h"
#include "DataCodec/Storage/ByteIO/Window/WindowRuntimeParams.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
namespace datacodec {

struct CacheResources {
    [[nodiscard]] DataCodecExecutionResources& Run() const {
        if (m_run == nullptr) {
            throw std::logic_error("cache resources require an active root");
        }
        return *m_run;
    }
    void BindRun(DataCodecExecutionResources& run) noexcept { m_run = &run; }
    void UnbindRun() noexcept { m_run = nullptr; }
    [[nodiscard]] std::shared_ptr<resource::ResidentByteBudget> StorageCapacity() const {
        if (m_run == nullptr) {
            throw std::logic_error("cache resources require an active root");
        }
        return m_run->StorageCapacity();
    }
    [[nodiscard]] bool ExternalSpillAvailable() const {
        if (m_run == nullptr) {
            throw std::logic_error("cache resources require an active root");
        }
        return m_run->ExternalSpillAvailable();
    }
    [[nodiscard]] ScratchByteBufferPool& ScratchBytePool() const {
        if (m_run == nullptr) {
            throw std::logic_error("scratch access requires an active root");
        }
        return m_run->Scratch();
    }
    template<typename TValue>
    [[nodiscard]] std::size_t ValuesPerWindow(const std::size_t valuesPerElement = 1u) const noexcept {
        return std::max<std::size_t>(
            1u,
            kIoWindowBytes /
                std::max<std::size_t>(sizeof(TValue) * valuesPerElement, sizeof(TValue)));
    }
private:
    DataCodecExecutionResources* m_run{nullptr};
};

} // namespace datacodec

#endif
