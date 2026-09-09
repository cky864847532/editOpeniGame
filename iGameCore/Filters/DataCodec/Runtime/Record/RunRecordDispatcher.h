#ifndef DATACODEC_RUNTIME_RECORD_RUNRECORDDISPATCHER_H
#define DATACODEC_RUNTIME_RECORD_RUNRECORDDISPATCHER_H

#include "DataCodec/API/Adapter/IRunRecordSink.h"

#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace datacodec {

class RunRecordDispatcher final : public IRunRecordSink {
public:
    void AddSink(IRunRecordSink* sink) {
        if (sink == nullptr) {
            return;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sinks.push_back(SinkEntry{.sink = sink});
    }

    void AddSink(std::shared_ptr<IRunRecordSink> sink) {
        if (sink == nullptr) {
            return;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        auto* raw = sink.get();
        m_sinks.push_back(SinkEntry{.owner = std::move(sink), .sink = raw});
    }

    [[nodiscard]] RunRecordMask Interests() const noexcept override {
        std::lock_guard<std::mutex> lock(m_mutex);
        RunRecordMask interests = 0u;
        for (const auto& entry : m_sinks) {
            interests |= entry.sink->Interests();
        }
        return interests;
    }

    [[nodiscard]] RunCollectionMask CollectionRequests() const noexcept override {
        std::lock_guard<std::mutex> lock(m_mutex);
        RunCollectionMask requests = 0u;
        for (const auto& entry : m_sinks) {
            requests |= entry.sink->CollectionRequests();
        }
        return requests;
    }

    void Submit(const RunRecord& record) override {
        const auto kind = GetRunRecordKind(record);
        std::size_t sinkCount = 0u;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            sinkCount = m_sinks.size();
        }
        // 逐个复制已有引用，锁外调用，单个消费者失败后继续交付其余消费者
        for (std::size_t index = 0u; index < sinkCount; ++index) {
            SinkEntry entry;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                entry = m_sinks[index];
            }
            if (entry.sink->Wants(kind) && !entry.sink->TrySubmit(record)) { RecordExportFailure(); }
        }
    }

private:
    struct SinkEntry {
        std::shared_ptr<IRunRecordSink> owner;
        IRunRecordSink* sink{nullptr};
    };

    mutable std::mutex m_mutex;
    std::vector<SinkEntry> m_sinks;
};

} // 命名空间 datacodec

#endif
