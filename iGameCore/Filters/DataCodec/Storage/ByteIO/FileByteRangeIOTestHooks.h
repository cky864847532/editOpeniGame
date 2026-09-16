#ifndef DATACODEC_STORAGE_BYTEIO_FILEBYTERANGEIOTESTHOOKS_H
#define DATACODEC_STORAGE_BYTEIO_FILEBYTERANGEIOTESTHOOKS_H

#include <cstddef>

namespace datacodec::fileiotest {

// 故障状态仅作用于当前线程，测试退出作用域后恢复原状态
enum class FailurePoint { None, Resize, Map, FlushMappedRange, FlushFile };
struct FailureState {
    FailurePoint point{FailurePoint::None};
    std::size_t successfulCalls{0u};
};

inline thread_local FailureState failureState;

inline bool ShouldFail(FailurePoint point) noexcept {
    if (failureState.point != point) { return false; }
    if (failureState.successfulCalls != 0u) { --failureState.successfulCalls; return false; }
    failureState.point = FailurePoint::None;
    return true;
}

class ScopedFailure final {
public:
    explicit ScopedFailure(FailurePoint point, std::size_t successfulCalls = 0u) noexcept
        : previous(failureState) { failureState = {point, successfulCalls}; }
    ~ScopedFailure() { failureState = previous; }
    ScopedFailure(const ScopedFailure&) = delete;
    ScopedFailure& operator=(const ScopedFailure&) = delete;
private:
    FailureState previous;
};

} // 命名空间 datacodec::fileiotest
#endif
