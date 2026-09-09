#include "DataCodec/Test/Common/DataCodecAllocationFailure.h"

#include <cstdlib>
#include <new>

#ifdef _WIN32
#include <malloc.h>
#endif

namespace datacodec::test {

thread_local bool rejectAllocations = false;
thread_local std::size_t rejectedAllocationCount = 0u;

} // DataCodec 测试命名空间

namespace {

void CheckAllocation() {
    if (datacodec::test::rejectAllocations) {
        ++datacodec::test::rejectedAllocationCount;
        throw std::bad_alloc{};
    }
}

} // 匿名命名空间

void* operator new(const std::size_t size) {
    CheckAllocation();
    if (auto* pointer = std::malloc(size == 0u ? 1u : size)) {
        return pointer;
    }
    throw std::bad_alloc{};
}

void* operator new[](const std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { ::operator delete(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { ::operator delete(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { ::operator delete(pointer); }

void* operator new(const std::size_t size, const std::align_val_t alignment) {
    CheckAllocation();
    void* pointer = nullptr;
#ifdef _WIN32
    pointer = _aligned_malloc(size == 0u ? 1u : size, static_cast<std::size_t>(alignment));
#else
    if (posix_memalign(&pointer, static_cast<std::size_t>(alignment), size == 0u ? 1u : size) != 0) {
        pointer = nullptr;
    }
#endif
    if (pointer != nullptr) {
        return pointer;
    }
    throw std::bad_alloc{};
}

void* operator new[](const std::size_t size, const std::align_val_t alignment) {
    return ::operator new(size, alignment);
}

void operator delete(void* pointer, std::align_val_t) noexcept {
#ifdef _WIN32
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}

void operator delete[](void* pointer, const std::align_val_t alignment) noexcept {
    ::operator delete(pointer, alignment);
}
void operator delete(void* pointer, std::size_t, const std::align_val_t alignment) noexcept {
    ::operator delete(pointer, alignment);
}
void operator delete[](void* pointer, std::size_t, const std::align_val_t alignment) noexcept {
    ::operator delete(pointer, alignment);
}

void* operator new(const std::size_t size, const std::nothrow_t&) noexcept {
    try { return ::operator new(size); } catch (...) { return nullptr; }
}
void* operator new[](const std::size_t size, const std::nothrow_t&) noexcept {
    try { return ::operator new[](size); } catch (...) { return nullptr; }
}
void operator delete(void* pointer, const std::nothrow_t&) noexcept { ::operator delete(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { ::operator delete[](pointer); }

void* operator new(const std::size_t size, const std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try { return ::operator new(size, alignment); } catch (...) { return nullptr; }
}
void* operator new[](const std::size_t size, const std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try { return ::operator new[](size, alignment); } catch (...) { return nullptr; }
}
void operator delete(void* pointer, const std::align_val_t alignment, const std::nothrow_t&) noexcept {
    ::operator delete(pointer, alignment);
}
void operator delete[](void* pointer, const std::align_val_t alignment, const std::nothrow_t&) noexcept {
    ::operator delete[](pointer, alignment);
}
