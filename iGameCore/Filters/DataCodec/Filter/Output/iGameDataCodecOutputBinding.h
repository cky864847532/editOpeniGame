#ifndef iGameDataCodeciGameDataCodecOutputBinding_h
#define iGameDataCodeciGameDataCodecOutputBinding_h

#include "DataCodec/Filter/Output/iGameDataCodecOutputSinks.h"
#include "DataCodec/Runtime/Output/DataCodecOutputRouter.h"
#include "DataCodec/Runtime/Record/RunRecordDispatcher.h"

#include <memory>
#include <atomic>
#include <utility>

IGAME_NAMESPACE_BEGIN

inline ::datacodec::DataCodecOutputSinks ResolveiGameDataCodecOutputSinks(
    ::datacodec::DataCodecOutputSinks sinks,
    const bool includeProgressBar,
    const bool includeConsole = true,
    bool* diagnosticsIncomplete = nullptr) noexcept {
    const auto createOptional = [&](auto&& create) noexcept {
        try { create(); }
        catch (...) { if (diagnosticsIncomplete) { *diagnosticsIncomplete = true; } }
    };
    if (includeConsole && sinks.console == nullptr) {
        createOptional([&] { sinks.console = std::make_shared<iGameSpdlogDataCodecConsoleSink>(); });
    }
    if (includeProgressBar && sinks.progress == nullptr) {
        createOptional([&] { sinks.progress = std::make_shared<iGameDataCodecProgressBarSink>(); });
    }
    return sinks;
}

class iGameDataCodecOutputBinding final {
public:
    iGameDataCodecOutputBinding(
        ::datacodec::DataCodecOutputSinks outputSinks,
        std::shared_ptr<::datacodec::IRunRecordSink> telemetrySink = {},
        const bool includeProgressBar = false,
        const bool includeConsole = true) noexcept
        : m_recordSink(std::move(telemetrySink)) {
        bool incomplete = false;
        m_outputSinks = ResolveiGameDataCodecOutputSinks(
            std::move(outputSinks), includeProgressBar, includeConsole, &incomplete);
        m_diagnosticsIncomplete.store(incomplete, std::memory_order_relaxed);
        if (m_outputSinks.Empty()) { return; }
        TryExport([&] {
            auto dispatcher = std::make_shared<::datacodec::RunRecordDispatcher>();
            dispatcher->AddSink(m_recordSink);
            dispatcher->AddSink(std::make_shared<::datacodec::DataCodecOutputRouter>(m_outputSinks));
            m_recordSink = std::move(dispatcher);
        });
    }

    [[nodiscard]] const ::datacodec::DataCodecOutputSinks& OutputSinks() const noexcept {
        return m_outputSinks;
    }

    [[nodiscard]] std::shared_ptr<::datacodec::IRunRecordSink> RecordSink() const {
        return m_recordSink;
    }

    template<class Function>
    bool TryExport(Function&& function) const noexcept {
        try {
            if (m_recordSink) {
                if (!m_recordSink->TryExport(std::forward<Function>(function))) {
                    m_diagnosticsIncomplete.store(true, std::memory_order_relaxed);
                    return false;
                }
            } else { std::forward<Function>(function)(); }
            return true;
        } catch (...) {
            m_diagnosticsIncomplete.store(true, std::memory_order_relaxed);
            return false;
        }
    }

    [[nodiscard]] bool DiagnosticsIncomplete() const noexcept {
        return m_diagnosticsIncomplete.load(std::memory_order_relaxed) ||
            (m_recordSink && m_recordSink->DiagnosticsIncomplete());
    }

private:
    ::datacodec::DataCodecOutputSinks m_outputSinks;
    std::shared_ptr<::datacodec::IRunRecordSink> m_recordSink;
    mutable std::atomic_bool m_diagnosticsIncomplete{false};
};

[[nodiscard]] inline std::shared_ptr<::datacodec::IRunRecordSink>
MakeiGameDataCodecOutputRecordSink(
    ::datacodec::DataCodecOutputSinks outputSinks = {},
    std::shared_ptr<::datacodec::IRunRecordSink> telemetrySink = {},
    const bool includeProgressBar = false,
    const bool includeConsole = true) {
    return iGameDataCodecOutputBinding(
        std::move(outputSinks),
        std::move(telemetrySink),
        includeProgressBar,
        includeConsole).RecordSink();
}

IGAME_NAMESPACE_END

#endif
