#ifndef DATACODEC_API_ADAPTER_IRUNRECORDSINK_H
#define DATACODEC_API_ADAPTER_IRUNRECORDSINK_H

#include "DataCodec/API/Adapter/RunRecord.h"
#include <atomic>
#include <utility>

namespace datacodec {

class IRunRecordSink {
public:
    virtual ~IRunRecordSink() = default;

    [[nodiscard]] virtual RunRecordMask Interests() const noexcept = 0;
    [[nodiscard]] virtual RunCollectionMask CollectionRequests() const noexcept {
        return 0u;
    }
    virtual void Submit(const RunRecord& record) = 0;

    // 将调用点的可选格式化纳入同一异常边界，计数独立于业务结果
    template<class Function>
    bool TryExport(Function&& function) const noexcept {
        const auto before = ExportFailureCount();
        try { std::forward<Function>(function)(); }
        catch (...) { RecordExportFailure(); }
        return ExportFailureCount() == before;
    }

    // 可选记录交付失败仅保留计数，不改变业务请求
    bool TrySubmit(const RunRecord& record) noexcept {
        const auto before = ExportFailureCount();
        try { Submit(record); }
        catch (...) { RecordExportFailure(); }
        return ExportFailureCount() == before;
    }

    [[nodiscard]] std::uint64_t ExportFailureCount() const noexcept {
        return m_exportFailures.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool DiagnosticsIncomplete() const noexcept { return ExportFailureCount() != 0u; }

    [[nodiscard]] bool Wants(const RunRecordKind kind) const noexcept {
        return (Interests() & RunRecordBit(kind)) != 0u;
    }

    [[nodiscard]] bool Requests(const RunCollectionKind kind) const noexcept {
        return (CollectionRequests() & RunCollectionBit(kind)) != 0u;
    }

protected:
    void RecordExportFailure() const noexcept { m_exportFailures.fetch_add(1u, std::memory_order_relaxed); }

private:
    mutable std::atomic_uint64_t m_exportFailures{0u};
};

} // 命名空间 datacodec

#endif
