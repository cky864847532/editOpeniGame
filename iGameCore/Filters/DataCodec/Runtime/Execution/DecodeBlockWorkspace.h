#ifndef DATACODEC_RUNTIME_EXECUTION_DECODEBLOCKWORKSPACE_H
#define DATACODEC_RUNTIME_EXECUTION_DECODEBLOCKWORKSPACE_H

#include "DataCodec/Runtime/Execution/DecodeBlockMemoryPlan.h"
#include "DataCodec/Storage/ByteIO/ScratchByteBuffer.h"

namespace datacodec {

class DecodeBlockWorkspace final {
public:
    DecodeBlockWorkspace(ScratchByteBufferPool& pool, DecodeBlockMemoryPlan plan)
        : m_pool(pool), m_plan(plan) {}
    DecodeBlockWorkspace(const DecodeBlockWorkspace&) = delete;
    DecodeBlockWorkspace& operator=(const DecodeBlockWorkspace&) = delete;
    ~DecodeBlockWorkspace() { for (auto& region : m_regions) { m_pool.ReturnFixed(std::move(region)); } }

    // 取出的 idle backing 仍持有预约，联合准入仅申请缺少部分
    std::uint64_t TakeReusable() {
        std::size_t missing = 0u;
        for (std::size_t i = 0u; i < m_regions.size(); ++i) {
            m_regions[i] = m_pool.TakeFixed(m_plan.regionBytes[i]);
            if (m_regions[i].size == 0u) { missing = DecodeBlockMemoryPlan::Add(missing, m_plan.regionBytes[i]); }
        }
        return missing;
    }
    void Allocate(resource::ResidentByteBudget& budget, resource::ResidentByteBudget::Lease lease) {
        for (std::size_t i = 0u; i < m_regions.size(); ++i) {
            if (m_regions[i].size != 0u || m_plan.regionBytes[i] == 0u) { continue; }
            auto part = lease.Split(m_plan.regionBytes[i]);
            if (!part) { throw std::logic_error("workspace admission does not match layout"); }
            m_regions[i] = m_pool.AllocateFixed(budget, std::move(*part));
        }
        if (lease.Bytes() != 0u) { throw std::logic_error("workspace admission has excess capacity"); }
    }
    template<class T>
    FixedArrayView<T> View(DecodeMemoryRange range) {
        auto storage = m_regions[static_cast<std::size_t>(range.region)].Span();
        if (range.offset > storage.size() || range.bytes > storage.size() - range.offset ||
            range.bytes % sizeof(T) != 0u || range.offset % alignof(T) != 0u) {
            throw std::length_error("decode memory view exceeds its region");
        }
        return FixedArrayView<T>({reinterpret_cast<T*>(storage.data() + range.offset), range.bytes / sizeof(T)});
    }
    std::unique_ptr<std::uint8_t[]> TransferOutput() noexcept { return m_regions[1].Transfer(); }
    const DecodeBlockMemoryPlan& Plan() const noexcept { return m_plan; }
private:
    ScratchByteBufferPool& m_pool;
    DecodeBlockMemoryPlan m_plan;
    std::array<FixedByteBacking, 3u> m_regions;
};

}
#endif
