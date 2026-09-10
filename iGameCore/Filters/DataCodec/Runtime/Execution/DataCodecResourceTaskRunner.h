#ifndef DATACODEC_RUNTIME_EXECUTION_DATACODECRESOURCETASKRUNNER_H
#define DATACODEC_RUNTIME_EXECUTION_DATACODECRESOURCETASKRUNNER_H

#include "DataCodec/Runtime/Execution/ParallelExecution.h"
#include "DataCodec/Validation/Common/DataCodecValidation.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace datacodec {

struct DataCodecResourceTaskRunnerStats {
    std::size_t workerLimit{0u};
    std::size_t runningTaskCount{0u};
    std::size_t peakRunningTaskCount{0u};
    std::size_t runningThreadCount{0u};
    std::size_t peakRunningThreadCount{0u};
    std::size_t queuedTaskCount{0u};
    std::size_t peakQueuedTaskCount{0u};
    std::uint64_t memoryLimitBytes{0u};
    std::uint64_t reservedMemoryBytes{0u};
    std::uint64_t peakReservedMemoryBytes{0u};
    std::uint64_t completedTaskCount{0u};
};

class DataCodecTaskClaimExceedsLimit final : public std::length_error {
public:
    DataCodecTaskClaimExceedsLimit(
        const std::uint64_t requestedBytes,
        const std::uint64_t limitBytes)
        : std::length_error(
              "DataCodec task memory claim exceeds limit; requestedBytes=" +
              std::to_string(requestedBytes) +
              "; limitBytes=" + std::to_string(limitBytes)) {}
};

class DataCodecTaskThreadClaimExceedsLimit final : public std::length_error {
public:
    DataCodecTaskThreadClaimExceedsLimit(
        const std::size_t requestedThreads,
        const std::size_t limitThreads)
        : std::length_error(
              "DataCodec task thread claim exceeds limit; requestedThreads=" +
              std::to_string(requestedThreads) +
              "; limitThreads=" + std::to_string(limitThreads)) {}
};

class DataCodecTaskResidentMemoryExceedsClaim final : public std::length_error {
public:
    DataCodecTaskResidentMemoryExceedsClaim(
        const std::uint64_t requestedBytes,
        const std::uint64_t availableBytes)
        : std::length_error(
              "DataCodec resident memory exceeds the current task claim; requestedBytes=" +
              std::to_string(requestedBytes) +
              "; availableBytes=" + std::to_string(availableBytes)) {}
};

namespace resource_task_runner_detail {

class RunnerState;
struct TaskReservationContext;

} // 资源任务运行器实现细节

class DataCodecTaskMemoryReservation final {
public:
    DataCodecTaskMemoryReservation() = default;
    DataCodecTaskMemoryReservation(const DataCodecTaskMemoryReservation&) = delete;
    DataCodecTaskMemoryReservation& operator=(const DataCodecTaskMemoryReservation&) = delete;

    DataCodecTaskMemoryReservation(DataCodecTaskMemoryReservation&& other) noexcept {
        MoveFrom(std::move(other));
    }

    DataCodecTaskMemoryReservation& operator=(DataCodecTaskMemoryReservation&& other) noexcept {
        if (this != &other) {
            Release();
            MoveFrom(std::move(other));
        }
        return *this;
    }

    ~DataCodecTaskMemoryReservation() {
        Release();
    }

    void Release() noexcept;

    void ShrinkTo(std::uint64_t bytes) noexcept;

    [[nodiscard]] DataCodecTaskMemoryReservation Split(std::uint64_t bytes);

    [[nodiscard]] std::uint64_t Bytes() const noexcept {
        return m_bytes;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return m_state != nullptr;
    }

private:
    friend class resource_task_runner_detail::RunnerState;
    friend DataCodecTaskMemoryReservation RetainCurrentDataCodecTaskMemoryReservation();
    friend DataCodecTaskMemoryReservation RetainCurrentDataCodecTaskMemoryReservation(
        std::uint64_t bytes);

    DataCodecTaskMemoryReservation(
        std::shared_ptr<resource_task_runner_detail::RunnerState> state,
        const std::uint64_t bytes,
        std::shared_ptr<resource_task_runner_detail::TaskReservationContext> taskContext = {},
        const std::uint64_t directBytes = 0u,
        const std::uint64_t borrowedBytes = 0u) noexcept
        : m_state(std::move(state)),
          m_bytes(bytes),
          m_directBytes(directBytes != 0u || borrowedBytes != 0u || taskContext != nullptr
                  ? directBytes
                  : bytes),
          m_borrowedBytes(borrowedBytes),
          m_taskContext(std::move(taskContext)) {}

    void MoveFrom(DataCodecTaskMemoryReservation&& other) noexcept {
        m_state = std::move(other.m_state);
        m_bytes = other.m_bytes;
        m_directBytes = other.m_directBytes;
        m_borrowedBytes = other.m_borrowedBytes;
        m_taskContext = std::move(other.m_taskContext);
        other.m_bytes = 0u;
        other.m_directBytes = 0u;
        other.m_borrowedBytes = 0u;
    }

    std::shared_ptr<resource_task_runner_detail::RunnerState> m_state;
    std::uint64_t m_bytes{0u};
    std::uint64_t m_directBytes{0u};
    std::uint64_t m_borrowedBytes{0u};
    std::shared_ptr<resource_task_runner_detail::TaskReservationContext> m_taskContext;
};

[[nodiscard]] DataCodecTaskMemoryReservation
RetainCurrentDataCodecTaskMemoryReservation();

[[nodiscard]] DataCodecTaskMemoryReservation
RetainCurrentDataCodecTaskMemoryReservation(std::uint64_t bytes);

class DataCodecMemoryReservationProvider final {
public:
    DataCodecMemoryReservationProvider() = default;

    explicit DataCodecMemoryReservationProvider(
        std::shared_ptr<resource_task_runner_detail::RunnerState> state) noexcept
        : m_state(std::move(state)) {}

    [[nodiscard]] DataCodecTaskMemoryReservation AcquireResidentMemoryReservation(
        std::uint64_t bytes,
        std::stop_token stopToken = {}) const;

    [[nodiscard]] DataCodecTaskMemoryReservation TryAcquireResidentMemoryReservation(
        std::uint64_t bytes) const;

    [[nodiscard]] std::uint64_t MemoryLimitBytes() const noexcept;

    [[nodiscard]] bool OwnsCurrentTaskReservation() const noexcept;

    [[nodiscard]] explicit operator bool() const noexcept {
        return m_state != nullptr;
    }

    [[nodiscard]] bool operator==(
        const DataCodecMemoryReservationProvider& other) const noexcept {
        return m_state == other.m_state;
    }

private:
    std::shared_ptr<resource_task_runner_detail::RunnerState> m_state;
};

class IDataCodecResourceTaskRunner : public IParallelTaskRunner {
public:
    ~IDataCodecResourceTaskRunner() override = default;
    [[nodiscard]] virtual DataCodecTaskMemoryReservation AcquireMemoryReservation(
        std::uint64_t bytes,
        std::stop_token stopToken = {}) = 0;
    [[nodiscard]] virtual DataCodecTaskMemoryReservation AcquireResidentMemoryReservation(
        std::uint64_t bytes,
        std::stop_token stopToken = {}) = 0;
    [[nodiscard]] virtual DataCodecMemoryReservationProvider ShareMemoryReservations() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t MemoryLimitBytes() const noexcept = 0;
    [[nodiscard]] virtual DataCodecResourceTaskRunnerStats SnapshotResourceStats() const noexcept = 0;
};

namespace resource_task_runner_detail {

inline thread_local RunnerState* g_currentRunnerState{nullptr};

struct TaskReservationContext {
    // 父任务等待子任务时由多个工作线程共同访问上下文
    mutable std::mutex mutex;
    RunnerState* state{nullptr};
    std::uint64_t bytes{0u};
    std::uint64_t directBytes{0u};
    std::uint64_t retainedBytes{0u};
    std::uint64_t retainedDirectBytes{0u};
    std::uint64_t retainedBorrowedBytes{0u};
    std::uint64_t lentBytes{0u};
    std::weak_ptr<TaskReservationContext> self;
    std::shared_ptr<TaskReservationContext> retentionParent;
    bool claimFinalized{false};
};

inline thread_local TaskReservationContext* g_currentReservationContext{nullptr};
inline thread_local DataCodecTaskMemoryReservation* g_residentReservationPool{nullptr};

struct TaskGroupState {
    mutable std::mutex mutex;
    std::condition_variable completed;
    std::size_t pendingTaskCount{0u};
    std::exception_ptr firstException;
};

struct QueuedTask {
    std::function<void()> task;
    DataCodecTaskResourceClaim resources;
    std::shared_ptr<TaskGroupState> group;
    std::stop_token stopToken;
    std::uint64_t sequence{0u};
    std::chrono::steady_clock::time_point queuedAt;
};

class RunnerState final : public std::enable_shared_from_this<RunnerState> {
public:
    using ExternalTaskSubmitter = std::function<std::future<void>(std::function<void()>)>;

    RunnerState(
        const std::size_t workerLimit,
        const std::uint64_t memoryLimitBytes,
        ExternalTaskSubmitter externalTaskSubmitter = {})
        : m_workerLimit(ClampDataCodecWorkerCount(workerLimit)),
          m_memoryLimitBytes(std::max<std::uint64_t>(memoryLimitBytes, 1u)),
          m_externalTaskSubmitter(std::move(externalTaskSubmitter)) {}

    ~RunnerState() {
        Shutdown();
    }

    void Start() {
        if (m_externalTaskSubmitter) {
            return;
        }
        m_workers.reserve(m_workerLimit);
        for (std::size_t workerIndex = 0u; workerIndex < m_workerLimit; ++workerIndex) {
            auto self = shared_from_this();
            m_workers.emplace_back([self = std::move(self), workerIndex](
                                        const std::stop_token stopToken) {
                self->WorkerLoop(workerIndex, stopToken);
            });
        }
    }

    void Shutdown() noexcept {
        std::deque<QueuedTask> rejected;
        std::vector<std::jthread> workers;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopping) {
                return;
            }
            m_stopping = true;
            rejected.swap(m_queue);
            for (auto& worker : m_workers) {
                worker.request_stop();
            }
            workers.swap(m_workers);
        }
        m_ready.notify_all();
        const auto exception = std::make_exception_ptr(
            std::runtime_error("DataCodec resource task runner stopped"));
        for (auto& task : rejected) {
            CompleteGroup(task.group, exception);
            task.task = {};
            task.group.reset();
        }
        rejected.clear();
        for (auto& worker : workers) {
            if (!worker.joinable()) {
                continue;
            }
            if (worker.get_id() == std::this_thread::get_id()) {
                worker.detach();
            } else {
                worker.join();
            }
        }
        if (m_externalTaskSubmitter) {
            const auto currentTaskCount = g_currentRunnerState == this ? 1u : 0u;
            std::unique_lock<std::mutex> lock(m_mutex);
            m_ready.wait(lock, [this, currentTaskCount]() {
                return m_runningTaskCount <= currentTaskCount;
            });
        }
    }

    void Submit(
        std::function<void()> task,
        const DataCodecTaskResourceClaim resources,
        const std::shared_ptr<TaskGroupState>& group,
        const std::stop_token stopToken) {
        if (!task) {
            return;
        }
        const auto normalizedThreadCount = NormalizeThreadCount(resources.threadCount);
        if (resources.memoryBytes > m_memoryLimitBytes) {
            throw DataCodecTaskClaimExceedsLimit(
                resources.memoryBytes,
                m_memoryLimitBytes);
        }
        if (normalizedThreadCount > m_workerLimit) {
            throw DataCodecTaskThreadClaimExceedsLimit(
                normalizedThreadCount,
                m_workerLimit);
        }
        {
            std::lock_guard<std::mutex> groupLock(group->mutex);
            ++group->pendingTaskCount;
        }
        try {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopping) {
                throw std::runtime_error("DataCodec resource task runner is stopped");
            }
            m_queue.push_back(QueuedTask{
                .task = std::move(task),
                .resources = DataCodecTaskResourceClaim{
                    .memoryBytes = resources.memoryBytes,
                    .threadCount = normalizedThreadCount,
                },
                .group = group,
                .stopToken = stopToken,
                .sequence = m_nextSequence++,
                .queuedAt = std::chrono::steady_clock::now(),
            });
            m_peakQueuedTaskCount = std::max(m_peakQueuedTaskCount, m_queue.size());
        } catch (...) {
            CompleteGroup(group, std::current_exception());
            throw;
        }
        m_ready.notify_all();
        DispatchExternalTasks();
    }

    void Wait(const std::shared_ptr<TaskGroupState>& group) {
        if (g_currentRunnerState == this) {
            HelpUntilComplete(group);
        } else {
            std::unique_lock<std::mutex> lock(group->mutex);
            group->completed.wait(lock, [&group]() {
                return group->pendingTaskCount == 0u;
            });
        }
        std::exception_ptr exception;
        {
            std::lock_guard<std::mutex> lock(group->mutex);
            exception = group->firstException;
        }
        if (exception != nullptr) {
            std::rethrow_exception(exception);
        }
    }

    void WaitUntil(
        const std::shared_ptr<TaskGroupState>& group,
        const std::function<bool()>& ready) {
        WorkerWaitScope workerWaitScope(*this);
        while (!ready()) {
            if (GroupComplete(group)) {
                return;
            }
            if (g_currentRunnerState == this) {
                std::optional<SelectedTask> selected;
                {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    selected = TakeTaskLocked(true);
                    if (!selected.has_value()) {
                        m_ready.wait(lock, [&]() {
                            return m_stopping || GroupComplete(group) || ready() ||
                                std::any_of(
                                    m_queue.begin(),
                                    m_queue.end(),
                                    [this](const QueuedTask& task) {
                                        return CanFitLocked(task, true);
                                    });
                        });
                        if (m_stopping) {
                            lock.unlock();
                            std::unique_lock<std::mutex> groupLock(group->mutex);
                            group->completed.wait(groupLock, [&group]() {
                                return group->pendingTaskCount == 0u;
                            });
                            return;
                        }
                        continue;
                    }
                }
                ExecuteSelectedTask(std::move(*selected));
                continue;
            }
            std::unique_lock<std::mutex> lock(group->mutex);
            group->completed.wait(
                lock,
                [&group, &ready]() {
                    return group->pendingTaskCount == 0u || ready();
                });
        }
    }

    [[nodiscard]] DataCodecResourceTaskRunnerStats Snapshot() const noexcept {
        std::lock_guard<std::mutex> lock(m_mutex);
        return DataCodecResourceTaskRunnerStats{
            .workerLimit = m_workerLimit,
            .runningTaskCount = m_runningTaskCount,
            .peakRunningTaskCount = m_peakRunningTaskCount,
            .runningThreadCount = UsedThreadCountLocked(),
            .peakRunningThreadCount = m_peakReservedThreadCount,
            .queuedTaskCount = m_queue.size(),
            .peakQueuedTaskCount = m_peakQueuedTaskCount,
            .memoryLimitBytes = m_memoryLimitBytes,
            .reservedMemoryBytes = m_reservedMemoryBytes,
            .peakReservedMemoryBytes = m_peakReservedMemoryBytes,
            .completedTaskCount = m_completedTaskCount,
        };
    }

    [[nodiscard]] std::size_t WorkerLimit() const noexcept {
        return m_workerLimit;
    }

    [[nodiscard]] std::uint64_t MemoryLimitBytes() const noexcept {
        return m_memoryLimitBytes;
    }

    void NotifyReady() noexcept {
        m_ready.notify_all();
        DispatchExternalTasks();
    }

    [[nodiscard]] bool TryRetainCurrentTaskMemory(
        TaskReservationContext& context,
        const std::uint64_t bytes,
        std::uint64_t* availableBytes = nullptr,
        std::uint64_t* directBytes = nullptr,
        std::uint64_t* borrowedBytes = nullptr) noexcept {
        std::lock_guard<std::mutex> runnerLock(m_mutex);
        std::lock_guard<std::mutex> contextLock(context.mutex);
        const auto available = TaskAvailableMemoryBytesLocked(context);
        if (availableBytes != nullptr) {
            *availableBytes = available;
        }
        if (context.state != this) {
            return false;
        }
        if (bytes > available) {
            const auto extraBytes = bytes - available;
            const auto globallyAvailableBytes =
                m_memoryLimitBytes - std::min(m_reservedMemoryBytes, m_memoryLimitBytes);
            if (extraBytes > globallyAvailableBytes) {
                return false;
            }
            m_reservedMemoryBytes += extraBytes;
            m_peakReservedMemoryBytes = std::max(
                m_peakReservedMemoryBytes,
                m_reservedMemoryBytes);
            context.bytes += extraBytes;
            context.directBytes += extraBytes;
        }
        const auto availableDirectBytes = context.directBytes - std::min(
            context.directBytes,
            context.retainedDirectBytes);
        const auto retainedDirectBytes = std::min(bytes, availableDirectBytes);
        const auto retainedBorrowedBytes = bytes - retainedDirectBytes;
        if (directBytes != nullptr) {
            *directBytes = retainedDirectBytes;
        }
        if (borrowedBytes != nullptr) {
            *borrowedBytes = retainedBorrowedBytes;
        }
        context.retainedBytes += bytes;
        context.retainedDirectBytes += retainedDirectBytes;
        context.retainedBorrowedBytes += retainedBorrowedBytes;
        return true;
    }

    [[nodiscard]] DataCodecTaskMemoryReservation AcquireMemoryReservation(
        const std::uint64_t bytes,
        const std::stop_token stopToken) {
        if (bytes == 0u) {
            return {};
        }
        if (bytes > m_memoryLimitBytes) {
            throw DataCodecTaskClaimExceedsLimit(bytes, m_memoryLimitBytes);
        }
        WorkerWaitScope workerWaitScope(*this);
        std::stop_callback stopCallback(stopToken, [this]() {
            m_ready.notify_all();
        });
        while (true) {
            std::optional<SelectedTask> selected;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                if (m_stopping) {
                    throw std::runtime_error("DataCodec resource task runner is stopped");
                }
                if (stopToken.stop_requested()) {
                    throw std::runtime_error("DataCodec memory reservation was cancelled");
                }
                const auto availableBytes =
                    m_memoryLimitBytes - std::min(m_reservedMemoryBytes, m_memoryLimitBytes);
                if (bytes <= availableBytes) {
                    m_reservedMemoryBytes += bytes;
                    m_peakReservedMemoryBytes = std::max(
                        m_peakReservedMemoryBytes,
                        m_reservedMemoryBytes);
                    return DataCodecTaskMemoryReservation(
                        shared_from_this(),
                        bytes);
                }
                if (g_currentRunnerState == this) {
                    selected = TakeTaskLocked(true);
                }
                if (!selected.has_value()) {
                    m_ready.wait(lock, [&]() {
                        const auto available = m_memoryLimitBytes - std::min(
                            m_reservedMemoryBytes,
                            m_memoryLimitBytes);
                        return m_stopping || stopToken.stop_requested() ||
                            bytes <= available ||
                            (g_currentRunnerState == this &&
                             std::any_of(
                                 m_queue.begin(),
                                 m_queue.end(),
                                 [this](const QueuedTask& task) {
                                     return CanFitLocked(task, true);
                                 }));
                    });
                    continue;
                }
            }
            ExecuteSelectedTask(std::move(*selected));
        }
    }

    [[nodiscard]] DataCodecTaskMemoryReservation AcquireResidentMemoryReservation(
        const std::uint64_t bytes,
        const std::stop_token stopToken) {
        if (bytes == 0u) {
            return {};
        }
        auto* reservationPool = g_residentReservationPool;
        if (reservationPool != nullptr &&
            reservationPool->m_state.get() == this) {
            if (bytes > reservationPool->Bytes()) {
                throw DataCodecTaskResidentMemoryExceedsClaim(
                    bytes,
                    reservationPool->Bytes());
            }
            return reservationPool->Split(bytes);
        }
        auto* context = g_currentReservationContext;
        if (context == nullptr || context->state != this || context->bytes == 0u) {
            return AcquireMemoryReservation(bytes, stopToken);
        }
        std::uint64_t availableBytes = 0u;
        std::uint64_t directBytes = 0u;
        std::uint64_t borrowedBytes = 0u;
        if (!TryRetainCurrentTaskMemory(
                *context,
                bytes,
                &availableBytes,
                &directBytes,
                &borrowedBytes)) {
            throw DataCodecTaskResidentMemoryExceedsClaim(bytes, availableBytes);
        }
        return DataCodecTaskMemoryReservation(
            shared_from_this(),
            bytes,
            context->self.lock(),
            directBytes,
            borrowedBytes);
    }

    [[nodiscard]] DataCodecTaskMemoryReservation TryAcquireResidentMemoryReservation(
        const std::uint64_t bytes) {
        if (bytes == 0u || bytes > m_memoryLimitBytes) {
            return {};
        }
        auto* reservationPool = g_residentReservationPool;
        if (reservationPool != nullptr &&
            reservationPool->m_state.get() == this) {
            return bytes <= reservationPool->Bytes()
                ? reservationPool->Split(bytes)
                : DataCodecTaskMemoryReservation{};
        }
        auto* context = g_currentReservationContext;
        if (context != nullptr && context->state == this && context->bytes != 0u) {
            std::uint64_t directBytes = 0u;
            std::uint64_t borrowedBytes = 0u;
            if (!TryRetainCurrentTaskMemory(
                    *context,
                    bytes,
                    nullptr,
                    &directBytes,
                    &borrowedBytes)) {
                return {};
            }
            return DataCodecTaskMemoryReservation(
                shared_from_this(),
                bytes,
                context->self.lock(),
                directBytes,
                borrowedBytes);
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) {
            return {};
        }
        const auto availableBytes =
            m_memoryLimitBytes - std::min(m_reservedMemoryBytes, m_memoryLimitBytes);
        if (bytes > availableBytes) {
            return {};
        }
        m_reservedMemoryBytes += bytes;
        m_peakReservedMemoryBytes = std::max(
            m_peakReservedMemoryBytes,
            m_reservedMemoryBytes);
        return DataCodecTaskMemoryReservation(shared_from_this(), bytes);
    }

    void ReleaseRetainedReservation(const std::uint64_t bytes) noexcept {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_reservedMemoryBytes = bytes >= m_reservedMemoryBytes
                ? 0u
                : m_reservedMemoryBytes - bytes;
        }
        m_ready.notify_all();
    }

    void ReleaseTaskRetainedReservation(
        const std::shared_ptr<TaskReservationContext>& context,
        const std::uint64_t directBytes,
        const std::uint64_t borrowedBytes) noexcept {
        if (context == nullptr || (directBytes == 0u && borrowedBytes == 0u)) {
            return;
        }
        {
            std::lock_guard<std::mutex> runnerLock(m_mutex);
            if (directBytes != 0u) {
                std::lock_guard<std::mutex> contextLock(context->mutex);
                const auto releasedBytes = std::min(
                    directBytes,
                    context->retainedDirectBytes);
                context->retainedDirectBytes -= releasedBytes;
                context->retainedBytes = releasedBytes >= context->retainedBytes
                    ? 0u
                    : context->retainedBytes - releasedBytes;
                if (context->claimFinalized) {
                    m_reservedMemoryBytes = releasedBytes >= m_reservedMemoryBytes
                        ? 0u
                        : m_reservedMemoryBytes - releasedBytes;
                }
            }

            std::uint64_t remainingBytes = borrowedBytes;
            auto current = context;
            bool transferredToParent = false;
            while (current != nullptr && remainingBytes != 0u) {
                std::shared_ptr<TaskReservationContext> parent;
                std::uint64_t releasedBytes = 0u;
                {
                    std::lock_guard<std::mutex> contextLock(current->mutex);
                    if (!transferredToParent) {
                        releasedBytes = std::min(
                            remainingBytes,
                            current->retainedBorrowedBytes);
                        current->retainedBorrowedBytes -= releasedBytes;
                    } else {
                        releasedBytes = std::min(
                            remainingBytes,
                            current->retainedDirectBytes);
                        current->retainedDirectBytes -= releasedBytes;
                    }
                    current->retainedBytes = releasedBytes >= current->retainedBytes
                        ? 0u
                        : current->retainedBytes - releasedBytes;
                    parent = current->retentionParent;
                    if (releasedBytes == 0u &&
                        !transferredToParent &&
                        current->claimFinalized &&
                        parent != nullptr) {
                        transferredToParent = true;
                    }
                }
                if (releasedBytes != 0u && current->claimFinalized) {
                    m_reservedMemoryBytes = releasedBytes >= m_reservedMemoryBytes
                        ? 0u
                        : m_reservedMemoryBytes - releasedBytes;
                }
                if (releasedBytes != 0u) {
                    remainingBytes -= releasedBytes;
                }
                current = std::move(parent);
            }
        }
        m_ready.notify_all();
    }

private:
    static constexpr auto kStarvationThreshold = std::chrono::milliseconds(250);
    static constexpr std::size_t kSelectionCandidateLimit = 64u;

    [[nodiscard]] static std::size_t NormalizeThreadCount(
        const std::size_t threadCount) noexcept {
        return std::max<std::size_t>(threadCount, 1u);
    }

    struct SelectedTask {
        QueuedTask task;
        std::size_t workerIndex{kInvalidParallelWorkerIndex};
        bool borrowedWorkerSlot{false};
        std::shared_ptr<TaskReservationContext> lendingContext;
        std::uint64_t borrowedMemoryBytes{0u};
        std::uint64_t reservedMemoryBytes{0u};
        std::size_t reservedThreadCount{0u};
        std::size_t borrowedThreadCount{0u};
        std::uint64_t retainedMemoryBytes{0u};
    };

    class WorkerWaitScope final {
    public:
        explicit WorkerWaitScope(RunnerState& state) noexcept
            : m_state(&state),
              m_active(g_currentRunnerState == &state) {
            if (!m_active) {
                return;
            }
            {
                std::lock_guard<std::mutex> lock(m_state->m_mutex);
                ++m_state->m_waitingWorkerCount;
            }
            m_state->m_ready.notify_all();
        }

        WorkerWaitScope(const WorkerWaitScope&) = delete;
        WorkerWaitScope& operator=(const WorkerWaitScope&) = delete;

        ~WorkerWaitScope() {
            if (!m_active || m_state == nullptr) {
                return;
            }
            {
                std::lock_guard<std::mutex> lock(m_state->m_mutex);
                if (m_state->m_waitingWorkerCount > 0u) {
                    --m_state->m_waitingWorkerCount;
                }
            }
            m_state->m_ready.notify_all();
        }

    private:
        RunnerState* m_state{nullptr};
        bool m_active{false};
    };

    [[nodiscard]] bool GroupComplete(
        const std::shared_ptr<TaskGroupState>& group) const noexcept {
        std::lock_guard<std::mutex> lock(group->mutex);
        return group->pendingTaskCount == 0u;
    }

    [[nodiscard]] std::uint64_t CurrentTaskAvailableMemoryBytes() const noexcept {
        const auto* context = g_currentReservationContext;
        if (context == nullptr || context->state != this) {
            return 0u;
        }
        std::lock_guard<std::mutex> contextLock(context->mutex);
        return TaskAvailableMemoryBytesLocked(*context);
    }

    [[nodiscard]] static std::uint64_t TaskAvailableMemoryBytesLocked(
        const TaskReservationContext& context) noexcept {
        const auto afterRetained = context.bytes - std::min(
            context.bytes,
            context.retainedBytes);
        return afterRetained - std::min(afterRetained, context.lentBytes);
    }

    [[nodiscard]] static std::uint64_t LendCurrentTaskMemoryLocked(
        TaskReservationContext& context,
        const std::uint64_t requestedBytes) noexcept {
        std::lock_guard<std::mutex> contextLock(context.mutex);
        const auto availableBytes = TaskAvailableMemoryBytesLocked(context);
        // 只允许整笔借用，避免子任务同时持有父任务和 runner 两种来源的内存
        if (requestedBytes > availableBytes) {
            return 0u;
        }
        context.lentBytes += requestedBytes;
        return requestedBytes;
    }

    [[nodiscard]] std::uint64_t AvailableMemoryBytesLocked(
        const bool borrowCurrentTaskMemory) const noexcept {
        const auto globallyAvailableBytes =
            m_memoryLimitBytes - std::min(m_reservedMemoryBytes, m_memoryLimitBytes);
        if (!borrowCurrentTaskMemory) {
            return globallyAvailableBytes;
        }
        const auto currentTaskAvailableBytes = CurrentTaskAvailableMemoryBytes();
        return validation::SaturatingAddU64(
            globallyAvailableBytes,
            currentTaskAvailableBytes);
    }

    [[nodiscard]] std::size_t AvailableThreadCountLocked(
        const bool borrowCurrentTaskThread) const noexcept {
        const auto used = UsedThreadCountLocked();
        const auto available = m_workerLimit - used;
        if (!borrowCurrentTaskThread) {
            return available;
        }
        const auto waitingWorkers = std::min(
            m_waitingWorkerCount,
            m_workerLimit - std::min(m_borrowedThreadCount, m_workerLimit));
        return available > m_workerLimit - waitingWorkers
            ? m_workerLimit
            : available + waitingWorkers;
    }

    [[nodiscard]] std::size_t UsedThreadCountLocked() const noexcept {
        const auto reserved = std::min(m_reservedThreadCount, m_workerLimit);
        const auto remaining = m_workerLimit - reserved;
        return reserved + std::min(m_borrowedThreadCount, remaining);
    }

    [[nodiscard]] bool CanFitLocked(
        const QueuedTask& task,
        const bool borrowCurrentTaskMemory) const noexcept {
        return task.stopToken.stop_requested() ||
            (NormalizeThreadCount(task.resources.threadCount) <=
                 AvailableThreadCountLocked(borrowCurrentTaskMemory) &&
             task.resources.memoryBytes <=
                 AvailableMemoryBytesLocked(borrowCurrentTaskMemory));
    }

    [[nodiscard]] std::deque<QueuedTask>::iterator SelectTaskLocked(
        const bool borrowCurrentTaskMemory) {
        const auto availableBytes = AvailableMemoryBytesLocked(
            borrowCurrentTaskMemory);
        const auto availableThreads = AvailableThreadCountLocked(
            borrowCurrentTaskMemory);
        const auto now = std::chrono::steady_clock::now();
        std::vector<std::deque<QueuedTask>::iterator> candidates;
        candidates.reserve(std::min(kSelectionCandidateLimit, m_queue.size()));
        for (auto iterator = m_queue.begin(); iterator != m_queue.end(); ++iterator) {
            if (NormalizeThreadCount(iterator->resources.threadCount) > availableThreads ||
                iterator->resources.memoryBytes > availableBytes) {
                continue;
            }
            candidates.push_back(iterator);
            if (candidates.size() == kSelectionCandidateLimit) {
                break;
            }
        }
        if (candidates.empty()) {
            return m_queue.end();
        }
        const auto oldestCandidate = candidates.front();
        const auto newestCandidate = *std::max_element(
            candidates.begin(),
            candidates.end(),
            [](const auto left, const auto right) {
                return left->queuedAt < right->queuedAt;
            });
        if (now - oldestCandidate->queuedAt >= kStarvationThreshold &&
            newestCandidate->queuedAt - oldestCandidate->queuedAt >=
                kStarvationThreshold) {
            return oldestCandidate;
        }

        // 在有限候选中寻找当前资源下最满的可运行组合
        std::vector<std::deque<QueuedTask>::iterator> ordered = candidates;
        std::sort(
            ordered.begin(),
            ordered.end(),
            [](const auto left, const auto right) {
                if (left->resources.memoryBytes != right->resources.memoryBytes) {
                    return left->resources.memoryBytes < right->resources.memoryBytes;
                }
                if (left->resources.threadCount != right->resources.threadCount) {
                    return left->resources.threadCount < right->resources.threadCount;
                }
                return left->sequence < right->sequence;
            });

        auto selected = m_queue.end();
        std::uint64_t selectedFillBytes = 0u;
        std::size_t selectedFillThreads = 0u;
        std::size_t selectedTaskCount = 0u;
        for (const bool smallestFirst : {true, false}) {
            if (!smallestFirst) {
                std::reverse(ordered.begin(), ordered.end());
            }
            for (const auto seed : candidates) {
                std::uint64_t fillBytes = seed->resources.memoryBytes;
                std::size_t fillThreads = NormalizeThreadCount(seed->resources.threadCount);
                std::vector<std::deque<QueuedTask>::iterator> packed{seed};
                for (const auto candidate : ordered) {
                    if (candidate == seed) {
                        continue;
                    }
                    const auto claim = candidate->resources.memoryBytes;
                    const auto taskThreads = NormalizeThreadCount(candidate->resources.threadCount);
                    if (claim > availableBytes - fillBytes ||
                        taskThreads > availableThreads - fillThreads) {
                        continue;
                    }
                    fillBytes += claim;
                    fillThreads += taskThreads;
                    packed.push_back(candidate);
                }

                auto firstTask = std::min_element(
                    packed.begin(),
                    packed.end(),
                    [](const auto left, const auto right) {
                        return left->sequence < right->sequence;
                    });
                const bool better = selected == m_queue.end() ||
                    fillBytes > selectedFillBytes ||
                    (fillBytes == selectedFillBytes &&
                     (fillThreads > selectedFillThreads ||
                      (fillThreads == selectedFillThreads &&
                       (packed.size() > selectedTaskCount ||
                        (packed.size() == selectedTaskCount &&
                         (*firstTask)->sequence < selected->sequence)))));
                if (better) {
                    selected = *firstTask;
                    selectedFillBytes = fillBytes;
                    selectedFillThreads = fillThreads;
                    selectedTaskCount = packed.size();
                }
            }
            if (!smallestFirst) {
                std::reverse(ordered.begin(), ordered.end());
            }
        }
        return selected;
    }

    [[nodiscard]] std::optional<SelectedTask> TakeTaskLocked(
        const bool borrowWorkerSlot) {
        const auto cancelled = std::find_if(
            m_queue.begin(),
            m_queue.end(),
            [](const QueuedTask& task) {
                return task.stopToken.stop_requested();
            });
        if (cancelled != m_queue.end()) {
            SelectedTask result{
                .task = std::move(*cancelled),
                .workerIndex = borrowWorkerSlot
                    ? CurrentParallelWorkerIndex()
                    : NextExternalWorkerIndexLocked(),
                .borrowedWorkerSlot = borrowWorkerSlot,
            };
            m_queue.erase(cancelled);
            if (!borrowWorkerSlot) {
                ++m_runningTaskCount;
                m_peakRunningTaskCount = std::max(
                    m_peakRunningTaskCount,
                    m_runningTaskCount);
            }
            return result;
        }
        const auto selected = SelectTaskLocked(borrowWorkerSlot);
        if (selected == m_queue.end()) {
            return std::nullopt;
        }
        SelectedTask result{
            .task = std::move(*selected),
            .workerIndex = borrowWorkerSlot
                ? CurrentParallelWorkerIndex()
                : NextExternalWorkerIndexLocked(),
            .borrowedWorkerSlot = borrowWorkerSlot,
        };
        m_queue.erase(selected);
        if (borrowWorkerSlot) {
            auto* context = g_currentReservationContext;
            if (context != nullptr && context->state == this) {
                auto lendingContext = context->self.lock();
                if (lendingContext != nullptr) {
                    const auto globallyAvailableBytes =
                        m_memoryLimitBytes - std::min(
                            m_reservedMemoryBytes,
                            m_memoryLimitBytes);
                    if (result.task.resources.memoryBytes > globallyAvailableBytes) {
                        result.borrowedMemoryBytes = LendCurrentTaskMemoryLocked(
                            *context,
                            result.task.resources.memoryBytes - globallyAvailableBytes);
                    }
                    if (result.borrowedMemoryBytes != 0u) {
                        result.lendingContext = std::move(lendingContext);
                    }
                }
            }
        }
        result.reservedMemoryBytes =
            result.task.resources.memoryBytes - result.borrowedMemoryBytes;
        result.reservedThreadCount = borrowWorkerSlot
            ? 0u
            : NormalizeThreadCount(result.task.resources.threadCount);
        result.borrowedThreadCount = borrowWorkerSlot
            ? NormalizeThreadCount(result.task.resources.threadCount)
            : 0u;
        m_reservedMemoryBytes += result.reservedMemoryBytes;
        m_peakReservedMemoryBytes = std::max(
            m_peakReservedMemoryBytes,
            m_reservedMemoryBytes);
        ++m_runningTaskCount;
        m_peakRunningTaskCount = std::max(
            m_peakRunningTaskCount,
            m_runningTaskCount);
        if (!borrowWorkerSlot) {
            m_reservedThreadCount += result.reservedThreadCount;
            m_peakReservedThreadCount = std::max(
                m_peakReservedThreadCount,
                UsedThreadCountLocked());
        }
        if (result.borrowedThreadCount != 0u) {
            m_borrowedThreadCount = validation::SaturatingAddSizeT(
                m_borrowedThreadCount,
                result.borrowedThreadCount);
            m_peakReservedThreadCount = std::max(
                m_peakReservedThreadCount,
                UsedThreadCountLocked());
        }
        return result;
    }

    void FinishTask(
        SelectedTask& selected,
        const std::shared_ptr<TaskReservationContext>& taskContext,
        std::exception_ptr exception) noexcept {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (taskContext != nullptr) {
                std::uint64_t releasedBytes = 0u;
                if (selected.lendingContext != nullptr &&
                    selected.lendingContext != taskContext) {
                    // 多层嵌套时把超出当前直接额度的保留量继续沿父链转移
                    std::scoped_lock contextLocks(
                        selected.lendingContext->mutex,
                        taskContext->mutex);
                    const auto retainedDirectBytesBeforeTransfer = taskContext->retainedDirectBytes;
                    const auto retainedBorrowedBytes = taskContext->retainedBorrowedBytes;
                    const auto inheritedDirectBytes = retainedDirectBytesBeforeTransfer > taskContext->directBytes
                        ? retainedDirectBytesBeforeTransfer - taskContext->directBytes
                        : 0u;
                    const auto transferBytes = validation::SaturatingAddU64(
                        retainedBorrowedBytes,
                        inheritedDirectBytes);
                    if (transferBytes != 0u) {
                        selected.lendingContext->retainedBytes = validation::SaturatingAddU64(
                            selected.lendingContext->retainedBytes,
                            transferBytes);
                        selected.lendingContext->retainedDirectBytes = validation::SaturatingAddU64(
                            selected.lendingContext->retainedDirectBytes,
                            transferBytes);
                        taskContext->retainedBytes = transferBytes >= taskContext->retainedBytes
                            ? 0u
                            : taskContext->retainedBytes - transferBytes;
                        taskContext->retainedDirectBytes = inheritedDirectBytes >=
                                taskContext->retainedDirectBytes
                            ? 0u
                            : taskContext->retainedDirectBytes - inheritedDirectBytes;
                        taskContext->retainedBorrowedBytes = 0u;
                        taskContext->retentionParent = selected.lendingContext;
                    }
                    taskContext->claimFinalized = true;
                    const auto retainedDirectBytes = std::min(
                        taskContext->directBytes,
                        taskContext->retainedDirectBytes);
                    releasedBytes = taskContext->directBytes - retainedDirectBytes;
                } else {
                    std::lock_guard<std::mutex> taskContextLock(taskContext->mutex);
                    if (taskContext->retainedBorrowedBytes != 0u) {
                        taskContext->retainedDirectBytes = validation::SaturatingAddU64(
                            taskContext->retainedDirectBytes,
                            taskContext->retainedBorrowedBytes);
                        taskContext->retainedBorrowedBytes = 0u;
                    }
                    taskContext->claimFinalized = true;
                    const auto retainedDirectBytes = std::min(
                        taskContext->directBytes,
                        taskContext->retainedDirectBytes);
                    releasedBytes = taskContext->directBytes - retainedDirectBytes;
                }
                if (releasedBytes != 0u) {
                    m_reservedMemoryBytes = releasedBytes >= m_reservedMemoryBytes
                        ? 0u
                        : m_reservedMemoryBytes - releasedBytes;
                }
            } else if (selected.reservedMemoryBytes != 0u) {
                m_reservedMemoryBytes = selected.reservedMemoryBytes >= m_reservedMemoryBytes
                    ? 0u
                    : m_reservedMemoryBytes - selected.reservedMemoryBytes;
            }
            if (selected.lendingContext != nullptr) {
                std::lock_guard<std::mutex> contextLock(selected.lendingContext->mutex);
                selected.lendingContext->lentBytes =
                    selected.borrowedMemoryBytes >= selected.lendingContext->lentBytes
                    ? 0u
                    : selected.lendingContext->lentBytes - selected.borrowedMemoryBytes;
            }
            if (m_runningTaskCount > 0u) {
                --m_runningTaskCount;
            }
            if (selected.reservedThreadCount != 0u) {
                m_reservedThreadCount = selected.reservedThreadCount >= m_reservedThreadCount
                    ? 0u
                    : m_reservedThreadCount - selected.reservedThreadCount;
            }
            if (selected.borrowedThreadCount != 0u) {
                m_borrowedThreadCount = selected.borrowedThreadCount >= m_borrowedThreadCount
                    ? 0u
                    : m_borrowedThreadCount - selected.borrowedThreadCount;
            }
            ++m_completedTaskCount;
        }
        CompleteGroup(selected.task.group, std::move(exception));
        m_ready.notify_all();
        DispatchExternalTasks();
    }

    [[nodiscard]] std::size_t NextExternalWorkerIndexLocked() noexcept {
        const auto workerIndex = m_nextWorkerIndex++;
        return workerIndex % std::max<std::size_t>(m_workerLimit, 1u);
    }

    void DispatchExternalTasks() noexcept {
        if (!m_externalTaskSubmitter) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopping || m_externalDispatching) {
                return;
            }
            m_externalDispatching = true;
        }
        while (true) {
            std::optional<SelectedTask> selected;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_stopping) {
                    m_externalDispatching = false;
                    return;
                }
                selected = TakeTaskLocked(false);
            }
            if (!selected.has_value()) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_externalDispatching = false;
                if (m_stopping || !std::any_of(
                        m_queue.begin(),
                        m_queue.end(),
                        [this](const QueuedTask& task) {
                            return CanFitLocked(task, false);
                        })) {
                    return;
                }
                m_externalDispatching = true;
                continue;
            }

            auto self = shared_from_this();
            auto task = std::make_shared<SelectedTask>(std::move(*selected));
            const auto group = task->task.group;
            try {
                auto future = m_externalTaskSubmitter(
                    [self, task]() mutable {
                        const auto previousRunnerState = g_currentRunnerState;
                        g_currentRunnerState = self.get();
                        ParallelWorkerIndexScope workerIndexScope(task->workerIndex);
                        self->ExecuteSelectedTask(std::move(*task));
                        g_currentRunnerState = previousRunnerState;
                    });
                if (!future.valid()) {
                    throw std::runtime_error(
                        "external DataCodec task runner rejected a task");
                }
            } catch (...) {
                task->task.group = group;
                FinishTask(*task, {}, std::current_exception());
            }
        }
    }

    void ExecuteSelectedTask(SelectedTask selected) noexcept {
        std::exception_ptr exception;
        auto reservationContext = std::make_shared<TaskReservationContext>();
        reservationContext->state = this;
        reservationContext->bytes = selected.task.resources.memoryBytes;
        reservationContext->directBytes = selected.reservedMemoryBytes;
        reservationContext->self = reservationContext;
        auto* previousReservationContext = g_currentReservationContext;
        auto* previousResidentReservationPool = g_residentReservationPool;
        g_currentReservationContext = reservationContext.get();
        g_residentReservationPool = nullptr;
        if (!selected.task.stopToken.stop_requested()) {
            try {
                selected.task.task();
            } catch (...) {
                exception = std::current_exception();
            }
        }
        g_residentReservationPool = previousResidentReservationPool;
        g_currentReservationContext = previousReservationContext;
        {
            std::lock_guard<std::mutex> contextLock(reservationContext->mutex);
            selected.retainedMemoryBytes = reservationContext->retainedBytes;
        }
        FinishTask(selected, reservationContext, std::move(exception));
    }

    static void CompleteGroup(
        const std::shared_ptr<TaskGroupState>& group,
        std::exception_ptr exception) noexcept {
        {
            std::lock_guard<std::mutex> lock(group->mutex);
            if (exception != nullptr && group->firstException == nullptr) {
                group->firstException = std::move(exception);
            }
            if (group->pendingTaskCount > 0u) {
                --group->pendingTaskCount;
            }
        }
        group->completed.notify_all();
    }

    void HelpUntilComplete(const std::shared_ptr<TaskGroupState>& group) {
        WorkerWaitScope workerWaitScope(*this);
        while (!GroupComplete(group)) {
            std::optional<SelectedTask> selected;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                selected = TakeTaskLocked(true);
                if (!selected.has_value()) {
                    m_ready.wait(lock, [&]() {
                        return m_stopping || GroupComplete(group) ||
                            std::any_of(
                                m_queue.begin(),
                                m_queue.end(),
                                [this](const QueuedTask& task) {
                                    return CanFitLocked(task, true);
                                });
                    });
                    if (m_stopping) {
                        lock.unlock();
                        std::unique_lock<std::mutex> groupLock(group->mutex);
                        group->completed.wait(groupLock, [&group]() {
                            return group->pendingTaskCount == 0u;
                        });
                        break;
                    }
                    continue;
                }
            }
            ExecuteSelectedTask(std::move(*selected));
        }
    }

    void WorkerLoop(
        const std::size_t workerIndex,
        const std::stop_token stopToken) noexcept {
        ParallelWorkerIndexScope workerIndexScope(workerIndex);
        auto* previousRunnerState = g_currentRunnerState;
        g_currentRunnerState = this;
        while (!stopToken.stop_requested()) {
            std::optional<SelectedTask> selected;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_ready.wait(lock, [&]() {
                    return m_stopping || stopToken.stop_requested() ||
                        std::any_of(
                            m_queue.begin(),
                            m_queue.end(),
                            [this](const QueuedTask& task) {
                                return CanFitLocked(task, false);
                            });
                });
                if (m_stopping || stopToken.stop_requested()) {
                    break;
                }
                selected = TakeTaskLocked(false);
            }
            if (selected.has_value()) {
                ExecuteSelectedTask(std::move(*selected));
            }
        }
        g_currentRunnerState = previousRunnerState;
    }

    const std::size_t m_workerLimit;
    const std::uint64_t m_memoryLimitBytes;
    ExternalTaskSubmitter m_externalTaskSubmitter;
    mutable std::mutex m_mutex;
    std::condition_variable m_ready;
    std::deque<QueuedTask> m_queue;
    std::vector<std::jthread> m_workers;
    std::size_t m_runningTaskCount{0u};
    std::size_t m_peakRunningTaskCount{0u};
    std::size_t m_reservedThreadCount{0u};
    std::size_t m_borrowedThreadCount{0u};
    std::size_t m_waitingWorkerCount{0u};
    std::size_t m_peakReservedThreadCount{0u};
    std::size_t m_peakQueuedTaskCount{0u};
    std::uint64_t m_reservedMemoryBytes{0u};
    std::uint64_t m_peakReservedMemoryBytes{0u};
    std::uint64_t m_completedTaskCount{0u};
    std::uint64_t m_nextSequence{0u};
    std::size_t m_nextWorkerIndex{0u};
    bool m_externalDispatching{false};
    bool m_stopping{false};
};

} // 资源任务运行器实现细节

inline DataCodecTaskMemoryReservation
DataCodecMemoryReservationProvider::AcquireResidentMemoryReservation(
    const std::uint64_t bytes,
    const std::stop_token stopToken) const {
    if (m_state == nullptr) {
        throw std::runtime_error("DataCodec memory reservation provider is unavailable");
    }
    return m_state->AcquireResidentMemoryReservation(bytes, stopToken);
}

inline DataCodecTaskMemoryReservation
DataCodecMemoryReservationProvider::TryAcquireResidentMemoryReservation(
    const std::uint64_t bytes) const {
    return m_state != nullptr
        ? m_state->TryAcquireResidentMemoryReservation(bytes)
        : DataCodecTaskMemoryReservation{};
}

inline std::uint64_t
DataCodecMemoryReservationProvider::MemoryLimitBytes() const noexcept {
    return m_state != nullptr ? m_state->MemoryLimitBytes() : 0u;
}

inline bool DataCodecMemoryReservationProvider::OwnsCurrentTaskReservation() const noexcept {
    const auto* context = resource_task_runner_detail::g_currentReservationContext;
    return m_state != nullptr &&
        context != nullptr &&
        context->state == m_state.get();
}

inline void DataCodecTaskMemoryReservation::Release() noexcept {
    if (m_state != nullptr) {
        if (m_taskContext != nullptr) {
            m_state->ReleaseTaskRetainedReservation(
                m_taskContext,
                m_directBytes,
                m_borrowedBytes);
        } else {
            m_state->ReleaseRetainedReservation(m_bytes);
        }
        m_state.reset();
    }
    m_bytes = 0u;
    m_directBytes = 0u;
    m_borrowedBytes = 0u;
    m_taskContext.reset();
}

inline void DataCodecTaskMemoryReservation::ShrinkTo(
    const std::uint64_t bytes) noexcept {
    if (m_state == nullptr || bytes >= m_bytes) {
        return;
    }
    const auto releasedBytes = m_bytes - bytes;
    const auto releasedDirectBytes = std::min(releasedBytes, m_directBytes);
    const auto releasedBorrowedBytes = releasedBytes - releasedDirectBytes;
    if (m_taskContext != nullptr) {
        m_state->ReleaseTaskRetainedReservation(
            m_taskContext,
            releasedDirectBytes,
            releasedBorrowedBytes);
    } else {
        m_state->ReleaseRetainedReservation(releasedBytes);
    }
    m_bytes = bytes;
    m_directBytes -= releasedDirectBytes;
    m_borrowedBytes -= releasedBorrowedBytes;
    if (m_bytes == 0u) {
        m_state.reset();
        m_directBytes = 0u;
        m_borrowedBytes = 0u;
        m_taskContext.reset();
    }
}

inline DataCodecTaskMemoryReservation DataCodecTaskMemoryReservation::Split(
    const std::uint64_t bytes) {
    if (bytes == 0u) {
        return {};
    }
    if (m_state == nullptr || bytes > m_bytes) {
        throw std::length_error(
            "split DataCodec memory reservation exceeds its available bytes");
    }
    auto state = m_state;
    auto taskContext = m_taskContext;
    const auto splitDirectBytes = std::min(bytes, m_directBytes);
    const auto splitBorrowedBytes = bytes - splitDirectBytes;
    m_bytes -= bytes;
    m_directBytes -= splitDirectBytes;
    m_borrowedBytes -= splitBorrowedBytes;
    if (m_bytes == 0u) {
        m_state.reset();
        m_directBytes = 0u;
        m_borrowedBytes = 0u;
        m_taskContext.reset();
    }
    return DataCodecTaskMemoryReservation(
        std::move(state),
        bytes,
        std::move(taskContext),
        splitDirectBytes,
        splitBorrowedBytes);
}

class DataCodecResidentMemoryReservationScope final {
public:
    explicit DataCodecResidentMemoryReservationScope(
        DataCodecTaskMemoryReservation& reservation) noexcept
        : m_active(static_cast<bool>(reservation)) {
        if (m_active) {
            m_previous = resource_task_runner_detail::g_residentReservationPool;
            resource_task_runner_detail::g_residentReservationPool = &reservation;
        }
    }

    DataCodecResidentMemoryReservationScope(
        const DataCodecResidentMemoryReservationScope&) = delete;
    DataCodecResidentMemoryReservationScope& operator=(
        const DataCodecResidentMemoryReservationScope&) = delete;

    ~DataCodecResidentMemoryReservationScope() {
        if (m_active) {
            resource_task_runner_detail::g_residentReservationPool = m_previous;
        }
    }

private:
    DataCodecTaskMemoryReservation* m_previous{nullptr};
    bool m_active{false};
};

[[nodiscard]] inline DataCodecTaskMemoryReservation
AcquireCurrentDataCodecScratchMemoryReservation(const std::uint64_t bytes) {
    auto* reservationPool = resource_task_runner_detail::g_residentReservationPool;
    if (reservationPool != nullptr) {
        return reservationPool->Split(bytes);
    }
    auto* context = resource_task_runner_detail::g_currentReservationContext;
    if (context == nullptr || context->state == nullptr || context->bytes == 0u) {
        auto* runnerState = resource_task_runner_detail::g_currentRunnerState;
        return runnerState != nullptr
            ? runnerState->AcquireMemoryReservation(bytes, {})
            : DataCodecTaskMemoryReservation{};
    }
    return RetainCurrentDataCodecTaskMemoryReservation(bytes);
}

[[nodiscard]] inline DataCodecTaskMemoryReservation
RetainCurrentDataCodecTaskMemoryReservation() {
    auto* context = resource_task_runner_detail::g_currentReservationContext;
    return context != nullptr
        ? RetainCurrentDataCodecTaskMemoryReservation(context->bytes)
        : DataCodecTaskMemoryReservation{};
}

[[nodiscard]] inline DataCodecTaskMemoryReservation
RetainCurrentDataCodecTaskMemoryReservation(const std::uint64_t bytes) {
    auto* context = resource_task_runner_detail::g_currentReservationContext;
    if (context == nullptr || context->state == nullptr || bytes == 0u) {
        return {};
    }
    std::uint64_t availableBytes = 0u;
    std::uint64_t directBytes = 0u;
    std::uint64_t borrowedBytes = 0u;
    if (!context->state->TryRetainCurrentTaskMemory(
            *context,
            bytes,
            &availableBytes,
            &directBytes,
            &borrowedBytes)) {
        throw std::length_error(
            "retained DataCodec task memory exceeds the current task claim; availableBytes=" +
                std::to_string(availableBytes));
    }
    return DataCodecTaskMemoryReservation(
        context->state->shared_from_this(),
        bytes,
        context->self.lock(),
        directBytes,
        borrowedBytes);
}

class DataCodecResourceTaskGroup final : public IDataCodecResourceTaskGroup {
public:
    DataCodecResourceTaskGroup(
        std::shared_ptr<resource_task_runner_detail::RunnerState> runner,
        const std::stop_token stopToken)
        : m_runner(std::move(runner)),
          m_state(std::make_shared<resource_task_runner_detail::TaskGroupState>()),
          m_stopToken(stopToken) {
        if (m_stopToken.stop_possible()) {
            m_stopCallback.emplace(
                m_stopToken,
                [runner = m_runner]() {
                    runner->NotifyReady();
                });
        }
    }

    ~DataCodecResourceTaskGroup() override {
        try {
            Wait();
        } catch (...) {
        }
    }

    void Submit(
        std::function<void()> task,
        const DataCodecTaskResourceClaim resources = {}) override {
        m_runner->Submit(
            std::move(task),
            resources,
            m_state,
            m_stopToken);
    }

    void Wait() override {
        m_runner->Wait(m_state);
    }

    void WaitUntil(const std::function<bool()>& ready) override {
        m_runner->WaitUntil(m_state, ready);
    }

private:
    std::shared_ptr<resource_task_runner_detail::RunnerState> m_runner;
    std::shared_ptr<resource_task_runner_detail::TaskGroupState> m_state;
    std::stop_token m_stopToken;
    std::optional<std::stop_callback<std::function<void()>>> m_stopCallback;
};

class DataCodecResourceTaskRunner final : public IDataCodecResourceTaskRunner {
public:
    DataCodecResourceTaskRunner(
        const std::size_t workerLimit,
        const std::uint64_t memoryLimitBytes,
        resource_task_runner_detail::RunnerState::ExternalTaskSubmitter externalTaskSubmitter = {})
        : m_state(std::make_shared<resource_task_runner_detail::RunnerState>(
              workerLimit,
              memoryLimitBytes,
              std::move(externalTaskSubmitter))) {
        m_state->Start();
    }

    ~DataCodecResourceTaskRunner() override {
        m_state->Shutdown();
    }

    [[nodiscard]] std::unique_ptr<IParallelTaskGroup> CreateGroup(
        const std::stop_token stopToken = {}) override {
        return std::make_unique<DataCodecResourceTaskGroup>(m_state, stopToken);
    }

    [[nodiscard]] std::size_t Concurrency() const noexcept override {
        return m_state->WorkerLimit();
    }

    [[nodiscard]] bool SupportsResourceClaims() const noexcept override {
        return true;
    }

    [[nodiscard]] DataCodecTaskMemoryReservation AcquireMemoryReservation(
        const std::uint64_t bytes,
        const std::stop_token stopToken = {}) override {
        return m_state->AcquireMemoryReservation(bytes, stopToken);
    }

    [[nodiscard]] DataCodecTaskMemoryReservation AcquireResidentMemoryReservation(
        const std::uint64_t bytes,
        const std::stop_token stopToken = {}) override {
        return m_state->AcquireResidentMemoryReservation(bytes, stopToken);
    }

    [[nodiscard]] DataCodecMemoryReservationProvider ShareMemoryReservations() const noexcept override {
        return DataCodecMemoryReservationProvider(m_state);
    }

    [[nodiscard]] std::uint64_t MemoryLimitBytes() const noexcept override {
        return m_state->MemoryLimitBytes();
    }

    [[nodiscard]] DataCodecResourceTaskRunnerStats SnapshotResourceStats() const noexcept override {
        return m_state->Snapshot();
    }

    [[nodiscard]] DataCodecResourceTaskRunnerStats SnapshotStats() const noexcept {
        return SnapshotResourceStats();
    }

private:
    std::shared_ptr<resource_task_runner_detail::RunnerState> m_state;
};

} // 命名空间 datacodec

#endif
