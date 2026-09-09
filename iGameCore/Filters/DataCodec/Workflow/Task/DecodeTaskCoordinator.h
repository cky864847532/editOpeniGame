#ifndef DATACODEC_WORKFLOW_TASK_DECODETASKCOORDINATOR_H
#define DATACODEC_WORKFLOW_TASK_DECODETASKCOORDINATOR_H

#include "DataCodec/Workflow/Task/DecodeTaskTypes.h"
#include "DataCodec/Runtime/Execution/DataCodecExecutionResources.h"

#include <array>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <variant>

namespace datacodec {

enum class DecodeCommandKind : std::uint8_t { Foreground, Prefetch };

template<class Result>
class DecodeTaskCoordinator final {
    struct Entry;
    struct SharedState {
        std::mutex mutex;
        std::condition_variable changed;
        std::array<std::shared_ptr<Entry>, 2u> pending;
        std::shared_ptr<Entry> current;
        std::thread::id driver;
        bool stopping{false};
    };
    struct Interest {
        explicit Interest(std::shared_ptr<Entry> entry) : target(std::move(entry)) {}
        ~Interest() {
            bool stop = false;
            {
                std::lock_guard lock(target->mutex);
                if (target->interestCount != 0u) { --target->interestCount; }
                stop = target->interestCount == 0u && !IsTerminalDecodeTaskState(target->status);
            }
            if (stop) { target->stopSource.request_stop(); }
        }
        std::shared_ptr<Entry> target;
    };
public:
    using Task = std::function<Result(std::stop_token)>;
    class Handle {
    public:
        Handle() = default;
        bool Valid() const noexcept { return m_entry != nullptr; }
        const DecodeTaskKey* Key() const noexcept { return m_entry ? &m_entry->key : nullptr; }
        DecodeTaskState State() const noexcept {
            if (!m_entry) { return DecodeTaskState::Cancelled; }
            std::lock_guard lock(m_entry->mutex);
            return m_entry->status;
        }
        bool Wait(Result& result, std::stop_token stop = {}) const {
            if (!m_entry) { return false; }
            if (auto state = m_entry->owner.lock()) {
                std::lock_guard lock(state->mutex);
                if (state->driver == std::this_thread::get_id()) {
                    throw std::logic_error("playback driver cannot wait for a command");
                }
            }
            std::unique_lock lock(m_entry->mutex);
            if (!m_entry->completed.wait(lock, stop, [&] { return IsTerminalDecodeTaskState(m_entry->status); })) { return false; }
            if (!m_entry->result) { return false; }
            result = *m_entry->result;
            return true;
        }
    private:
        friend class DecodeTaskCoordinator;
        Handle(std::shared_ptr<Entry> entry, std::shared_ptr<Interest> interest)
            : m_entry(std::move(entry)), m_interest(std::move(interest)) {}
        std::shared_ptr<Entry> m_entry;
        std::shared_ptr<Interest> m_interest;
    };

    explicit DecodeTaskCoordinator(DataCodecExecutionResources& run)
        : m_run(run), m_state(std::make_shared<SharedState>()) {
        static_assert(std::is_nothrow_move_constructible_v<Result>);
        if (run.Threaded()) {
            try { m_driver = std::thread([this] { DriverMain(); }); }
            catch (...) {
                m_run.RecordFailure(CommandFailure("driver-create-failed", "playback driver creation failed"), true);
                throw;
            }
        }
    }
    DecodeTaskCoordinator(const DecodeTaskCoordinator&) = delete;
    DecodeTaskCoordinator& operator=(const DecodeTaskCoordinator&) = delete;
    ~DecodeTaskCoordinator() { Stop(); }

    Handle Submit(const DecodeTaskKey& key, Task task, DecodeCommandKind kind = DecodeCommandKind::Foreground) try {
        if (!task) { throw std::invalid_argument("decode command must not be empty"); }
        auto entry = std::make_shared<Entry>();
        entry->key = key;
        entry->task = std::move(task);
        entry->owner = m_state;
        auto interest = std::make_shared<Interest>(entry);
        bool rejected = false;
        bool stopped = false;
        {
            std::lock_guard lock(m_state->mutex);
            const auto duplicate = [&](const std::shared_ptr<Entry>& existing) -> std::optional<Handle> {
                if (!existing || existing->key != key) { return std::nullopt; }
                std::lock_guard entryLock(existing->mutex);
                if (existing->interestCount == 0u && existing->stopSource.stop_requested()) { return std::nullopt; }
                auto sharedInterest = std::make_shared<Interest>(existing);
                ++existing->interestCount;
                return Handle(existing, std::move(sharedInterest));
            };
            if (auto handle = duplicate(m_state->current)) { return std::move(*handle); }
            for (const auto& pending : m_state->pending) {
                if (auto handle = duplicate(pending)) { return std::move(*handle); }
            }
            stopped = m_state->stopping || m_state->driver == std::this_thread::get_id();
            const auto lane = kind == DecodeCommandKind::Foreground ? 0u : 1u;
            rejected = m_state->pending[lane] != nullptr;
            if (!stopped && !rejected) { m_state->pending[lane] = entry; }
        }
        if (stopped || rejected) {
            Result result{};
            SetFailure(result, CommandFailure(stopped ? "command-not-accepted" : "command-window-full",
                stopped ? "playback command cannot be accepted" : "待运行请求窗口已满"));
            entry->task = {};
            Publish(entry, std::move(result));
        } else {
            m_state->changed.notify_all();
            if (!m_run.Threaded()) { ExecuteNext(); }
        }
        return Handle(std::move(entry), std::move(interest));
    } catch (const std::bad_alloc&) {
        m_run.RecordFailure(CommandFailure("allocation-failed", "command submission allocation failed"), true);
        CancelAll();
        throw;
    } catch (const std::exception& error) {
        m_run.RecordFailure(CommandFailure("command-submit-failed", error.what()), true);
        CancelAll();
        throw;
    } catch (...) {
        m_run.RecordFailure(CommandFailure("command-submit-failed", "command submission failed"), true);
        CancelAll();
        throw;
    }

    void CancelAll() noexcept {
        std::array<std::shared_ptr<Entry>, 3u> entries;
        {
            std::lock_guard lock(m_state->mutex);
            entries = {m_state->current, m_state->pending[0], m_state->pending[1]};
        }
        for (const auto& entry : entries) { if (entry) { entry->stopSource.request_stop(); } }
        m_state->changed.notify_all();
    }
    void WaitIdle() {
        std::unique_lock lock(m_state->mutex);
        if (m_state->driver == std::this_thread::get_id()) {
            throw std::logic_error("playback driver cannot wait for itself");
        }
        m_state->changed.wait(lock, [&] {
            return !m_state->current && !m_state->pending[0] && !m_state->pending[1];
        });
    }
    bool IsDriverThread() const noexcept {
        std::lock_guard lock(m_state->mutex);
        return m_state->driver == std::this_thread::get_id();
    }
    std::size_t Concurrency() const noexcept { return m_run.Concurrency(); }
    std::size_t InFlightTaskCount() const noexcept {
        std::lock_guard lock(m_state->mutex);
        return static_cast<std::size_t>(m_state->current != nullptr) +
            static_cast<std::size_t>(m_state->pending[0] != nullptr) +
            static_cast<std::size_t>(m_state->pending[1] != nullptr);
    }

private:
    struct Entry {
        DecodeTaskKey key;
        Task task;
        std::stop_source stopSource;
        std::weak_ptr<SharedState> owner;
        mutable std::mutex mutex;
        std::condition_variable_any completed;
        std::optional<Result> result;
        DecodeTaskState status{DecodeTaskState::Queued};
        std::size_t interestCount{1u};
    };
    static CodecFailureRecord CommandFailure(std::string_view reason, std::string_view text) noexcept {
        return MakeCodecFailureRecord(CodecErrorCode::PipelineFailure, reason, "DecodeTaskCoordinator", text);
    }
    template<class Function>
    static decltype(auto) Visit(Result& result, Function&& function) noexcept {
        if constexpr (requires { result.success; }) { return function(result); }
        else { return std::visit(std::forward<Function>(function), result); }
    }
    static void SetFailure(Result& result, const CodecFailureRecord& failure) noexcept {
        Visit(result, [&](auto& value) {
            value.success = false;
            value.cancelled = failure.cancelled;
            value.failure = failure;
        });
    }
    static bool Successful(Result& result) noexcept {
        return Visit(result, [](auto& value) { return value.success; });
    }
    static void Publish(const std::shared_ptr<Entry>& entry, Result result) noexcept {
        {
            std::lock_guard lock(entry->mutex);
            const bool success = Successful(result);
            const bool cancelled = Visit(result, [](auto& value) { return value.cancelled; });
            entry->result.emplace(std::move(result));
            entry->status = success ? DecodeTaskState::Succeeded : cancelled ? DecodeTaskState::Cancelled : DecodeTaskState::Failed;
        }
        entry->completed.notify_all();
    }

    void ExecuteNext() noexcept {
        std::shared_ptr<Entry> entry;
        Task task;
        {
            std::lock_guard lock(m_state->mutex);
            const auto lane = m_state->pending[0] ? 0u : 1u;
            if (m_state->current || !m_state->pending[lane]) { return; }
            entry = std::move(m_state->pending[lane]);
            m_state->current = entry;
            m_state->driver = std::this_thread::get_id();
            task = std::move(entry->task);
        }
        Result result{};
        if (entry->stopSource.stop_requested()) {
            auto failure = CommandFailure("cancelled", "command cancelled before execution");
            failure.cancelled = true;
            SetFailure(result, failure);
        } else {
            CodecRunScope run(m_run);
            if (!run) {
                SetFailure(result, m_run.FirstFailure().value_or(CommandFailure("request-start-failed", "request could not start")));
            } else {
                {
                    std::lock_guard lock(entry->mutex);
                    entry->status = DecodeTaskState::Running;
                }
                {
                    std::stop_callback cancel(entry->stopSource.get_token(), [&] { m_run.RequestStop(); });
                    try {
                        result = task(m_run.StopToken());
                        Visit(result, [&](auto& value) { if (value.failure) { m_run.RecordFailure(*value.failure); } });
                    } catch (const std::bad_alloc&) {
                        m_run.RecordFailure(CommandFailure("allocation-failed", "command allocation failed"));
                    } catch (const std::exception& error) {
                        m_run.RecordFailure(CommandFailure("command-exception", error.what()));
                    } catch (...) {
                        m_run.RecordFailure(CommandFailure("command-exception", "unknown command exception"));
                    }
                    task = {};
                    if (!Successful(result) && !m_run.FirstFailure()) {
                        m_run.RecordFailure(CommandFailure("command-failed", "decode command failed"));
                    }
                    if (!run.Finish(Successful(result))) {
                        SetFailure(result, m_run.FirstFailure().value_or(CommandFailure("command-failed", "decode command failed")));
                    }
                }
            }
        }
        task = {};
        Publish(entry, std::move(result));
        {
            std::lock_guard lock(m_state->mutex);
            m_state->current.reset();
            if (!m_run.Threaded()) { m_state->driver = {}; }
        }
        m_state->changed.notify_all();
    }
    void DriverMain() noexcept {
        {
            std::lock_guard lock(m_state->mutex);
            m_state->driver = std::this_thread::get_id();
        }
        for (;;) {
            {
                std::unique_lock lock(m_state->mutex);
                m_state->changed.wait(lock, [&] { return m_state->stopping || m_state->pending[0] || m_state->pending[1]; });
                if (m_state->stopping && !m_state->pending[0] && !m_state->pending[1]) { break; }
            }
            ExecuteNext();
        }
        {
            std::lock_guard lock(m_state->mutex);
            m_state->driver = {};
        }
        m_state->changed.notify_all();
    }
    void Stop() noexcept {
        {
            std::lock_guard lock(m_state->mutex);
            m_state->stopping = true;
        }
        CancelAll();
        if (m_driver.joinable()) { m_driver.join(); }
    }

    DataCodecExecutionResources& m_run;
    std::shared_ptr<SharedState> m_state;
    std::thread m_driver;
};

}

#endif
