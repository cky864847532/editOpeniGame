#ifndef DATACODEC_TEST_COMMON_DATACODECALLOCATIONFAILURE_H
#define DATACODEC_TEST_COMMON_DATACODECALLOCATIONFAILURE_H

#include <cstddef>

namespace datacodec::test {

// 仅测试程序链接分配拦截，拒绝当前线程之后的全部 C++ 堆申请
extern thread_local bool rejectAllocations;
extern thread_local std::size_t rejectedAllocationCount;

class RejectAllocationsScope final {
public:
    RejectAllocationsScope() noexcept : m_previous(rejectAllocations) {
        rejectAllocations = true;
        rejectedAllocationCount = 0u;
    }

    ~RejectAllocationsScope() { rejectAllocations = m_previous; }
    RejectAllocationsScope(const RejectAllocationsScope&) = delete;
    RejectAllocationsScope& operator=(const RejectAllocationsScope&) = delete;

private:
    bool m_previous;
};

} // DataCodec 测试命名空间

#endif
