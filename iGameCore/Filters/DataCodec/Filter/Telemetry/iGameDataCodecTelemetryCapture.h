#ifndef iGameDataCodeciGameDataCodecTelemetryCapture_h
#define iGameDataCodeciGameDataCodecTelemetryCapture_h

#include "DataCodec/Log/Capture/RemapOrderCapture.h"
#include "DataCodec/Log/Telemetry/Sinks/TelemetrySessionSink.h"
#include "DataCodec/Runtime/Record/RunRecordDispatcher.h"

#include <memory>
#include <atomic>
#include <utility>
#include <vector>

IGAME_NAMESPACE_BEGIN

class iGameDataCodecTelemetryCapture final {
public:
    explicit iGameDataCodecTelemetryCapture(
        std::shared_ptr<::datacodec::IRunRecordSink> downstream = {}) noexcept
        : m_downstream(std::move(downstream)) {}

    iGameDataCodecTelemetryCapture(iGameDataCodecTelemetryCapture&& other) noexcept
        : m_downstream(std::move(other.m_downstream)),
          m_diagnosticsIncomplete(other.DiagnosticsIncomplete()),
          m_dispatcher(std::move(other.m_dispatcher)),
          m_sessionCapture(std::move(other.m_sessionCapture)),
          m_remapCapture(std::move(other.m_remapCapture)) {}

    void MarkDiagnosticsIncomplete() const noexcept {
        m_diagnosticsIncomplete.store(true, std::memory_order_relaxed);
    }

    void CaptureSessions(
        const ::datacodec::RunRecordMask interests,
        const ::datacodec::RunCollectionMask collectionRequests = 0u,
        const ::datacodec::TelemetrySessionDetail detail =
            ::datacodec::TelemetrySessionDetail::Full) noexcept {
        if (m_sessionCapture != nullptr) {
            return;
        }
        TryExport([&] {
            auto capture = std::make_shared<::datacodec::TelemetrySessionSink>(
                interests, collectionRequests, detail);
            EnsureDispatcher();
            m_dispatcher->AddSink(capture);
            m_sessionCapture = std::move(capture);
        });
    }

    void CaptureRemapOrders() {
        if (m_remapCapture != nullptr) {
            return;
        }
        // 明确请求的分析是必要操作，创建失败交给调用方的 failure 边界
        auto capture = std::make_shared<::datacodec::log::RemapOrderCapture>();
        EnsureDispatcher();
        m_dispatcher->AddSink(capture);
        m_remapCapture = std::move(capture);
    }

    [[nodiscard]] std::shared_ptr<::datacodec::IRunRecordSink> Sink() const {
        return m_dispatcher ? std::static_pointer_cast<::datacodec::IRunRecordSink>(m_dispatcher) : m_downstream;
    }

    template<class Function>
    bool TryExport(Function&& function) const noexcept {
        try { std::forward<Function>(function)(); return true; }
        catch (...) { m_diagnosticsIncomplete.store(true, std::memory_order_relaxed); return false; }
    }

    [[nodiscard]] bool DiagnosticsIncomplete() const noexcept {
        return m_diagnosticsIncomplete.load(std::memory_order_relaxed) ||
            (m_dispatcher && m_dispatcher->DiagnosticsIncomplete()) ||
            (m_downstream && m_downstream->DiagnosticsIncomplete());
    }

    [[nodiscard]] std::vector<::datacodec::TelemetryMessageRecord> SnapshotMessages() const {
        return m_sessionCapture != nullptr
            ? m_sessionCapture->SnapshotMessages()
            : std::vector<::datacodec::TelemetryMessageRecord>{};
    }

    [[nodiscard]] std::vector<::datacodec::TelemetrySession>
    SnapshotCompletedTelemetrySessions() const {
        return m_sessionCapture != nullptr
            ? m_sessionCapture->SnapshotCompletedSessions()
            : std::vector<::datacodec::TelemetrySession>{};
    }

    [[nodiscard]] ::datacodec::log::RemapOrderSnapshot TakeRemapOrders() const {
        return m_remapCapture != nullptr
            ? m_remapCapture->TakeSnapshot()
            : ::datacodec::log::RemapOrderSnapshot{};
    }

private:
    void EnsureDispatcher() {
        if (m_dispatcher) { return; }
        auto dispatcher = std::make_shared<::datacodec::RunRecordDispatcher>();
        dispatcher->AddSink(m_downstream);
        m_dispatcher = std::move(dispatcher);
    }

    std::shared_ptr<::datacodec::IRunRecordSink> m_downstream;
    mutable std::atomic_bool m_diagnosticsIncomplete{false};
    std::shared_ptr<::datacodec::RunRecordDispatcher> m_dispatcher;
    std::shared_ptr<::datacodec::TelemetrySessionSink> m_sessionCapture;
    std::shared_ptr<::datacodec::log::RemapOrderCapture> m_remapCapture;
};

IGAME_NAMESPACE_END

#endif
