#ifndef DATACODEC_TEST_FEATURE_DATACODECFEATUREBYTERANGE_H
#define DATACODEC_TEST_FEATURE_DATACODECFEATUREBYTERANGE_H

#include "DataCodec/Test/Feature/DataCodecFeatureInputCancellation.h"

#include "DataCodec/Storage/ByteIO/CallbackByteRangeReader.h"
#include "DataCodec/Test/Common/DataCodecTestResult.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace datacodec::test {

[[nodiscard]] inline TestResult RunDataCodecFeatureByteRange() noexcept {
    TestResult result;
    static_assert(!std::is_constructible_v<MemoryByteRangeReader, std::span<const std::uint8_t>>);
    static_assert(!std::is_constructible_v<MemoryByteRangeReader, std::vector<std::uint8_t>>);
    {
        auto owner = std::make_shared<const std::vector<std::uint8_t>>(8u, 23u);
        std::weak_ptr<const std::vector<std::uint8_t>> lifetime = owner;
        const auto* original = owner->data();
        auto retained = std::make_shared<MemoryByteRangeReader>(owner, std::span(*owner).subspan(2u, 4u));
        owner.reset();
        Require(result, !lifetime.expired() && retained->ContiguousRange(0u, 4u).data() == original + 2u,
            "byteRange.retained-input", "the input must retain its original allocation without copying");
        SubrangeByteRangeReader range(retained, 1u, 2u);
        retained.reset();
        std::array<std::uint8_t, 2u> bytes{};
        Require(result, !lifetime.expired() && range.ReadAt(0u, bytes) && bytes[0] == 23u,
            "byteRange.retained-subrange", "a range must retain the original owner after the input reader is released");
    }
    {
        const std::array<std::uint8_t, 1u> bytes{1u};
        bool rejected = false;
        try { MemoryByteRangeReader missing(std::shared_ptr<const void>{}, bytes); }
        catch (const std::invalid_argument&) { rejected = true; }
        Require(result, rejected, "byteRange.missing-owner", "a nonempty span requires a retained owner");
    }
    std::uint64_t observedOffset = 0u;
    std::size_t observedSize = 0u;
    std::size_t callbackCount = 0u;
    CallbackByteRangeReader reader(
        64u,
        [&](const std::uint64_t offset,
            const std::span<std::uint8_t> output,
            std::string*) {
            ++callbackCount;
            observedOffset = offset;
            observedSize = output.size();
            for (std::size_t index = 0u; index < output.size(); ++index) {
                output[index] = static_cast<std::uint8_t>(offset + index);
            }
            return true;
        });

    std::array<std::uint8_t, 4u> bytes{};
    std::string error;
    const auto read = reader.ReadAt(9u, bytes, &error);
    Require(result, read, "byteRange.callback.read", error.empty() ? "callback read failed" : error);
    Require(result, reader.ByteSize() == 64u, "byteRange.callback.byteSize", "byte size mismatch");
    Require(result, callbackCount == 1u, "byteRange.callback.count", "callback count mismatch");
    Require(result, observedOffset == 9u, "byteRange.callback.offset", "offset was not forwarded");
    Require(result, observedSize == bytes.size(), "byteRange.callback.size", "size was not forwarded");
    Require(
        result,
        bytes == std::array<std::uint8_t, 4u>{9u, 10u, 11u, 12u},
        "byteRange.callback.bytes",
        "callback output bytes mismatch");

    error.clear();
    const auto invalidRead = reader.ReadAt(62u, bytes, &error);
    Require(result, !invalidRead, "byteRange.callback.bounds", "out-of-range read was accepted");
    Require(result, !error.empty(), "byteRange.callback.boundsError", "out-of-range read did not report an error");
    Require(result, callbackCount == 1u, "byteRange.callback.boundsCount", "invalid read invoked the callback");

    error.clear();
    const auto emptyRead = reader.ReadAt(64u, std::span<std::uint8_t>{}, &error);
    Require(result, emptyRead, "byteRange.callback.empty", error.empty() ? "empty tail read failed" : error);
    Require(result, callbackCount == 1u, "byteRange.callback.emptyCount", "empty read invoked the callback");

    CallbackByteRangeReader missingCallbackReader(8u, {});
    std::array<std::uint8_t, 1u> missingBytes{};
    error.clear();
    const auto missingRead = missingCallbackReader.ReadAt(0u, missingBytes, &error);
    Require(result, !missingRead, "byteRange.callback.missing", "missing callback read was accepted");
    Require(result, !error.empty(), "byteRange.callback.missingError", "missing callback did not report an error");

    auto memoryOwner = std::make_shared<const std::vector<std::uint8_t>>(
        std::initializer_list<std::uint8_t>{1u, 2u, 3u, 4u});
    MemoryByteRangeReader memoryReader(memoryOwner);
    std::span<const std::uint8_t> contiguousBytes;
    error.clear();
    const auto contiguousReady = memoryReader.PrepareContiguousRange(
        1u,
        2u,
        contiguousBytes,
        &error);
    Require(
        result,
        contiguousReady == ContiguousViewStatus::Ready &&
            contiguousBytes.size() == 2u &&
            contiguousBytes[0] == 2u &&
            contiguousBytes[1] == 3u,
        "byteRange.contiguous.ready",
        error.empty() ? "memory contiguous view was not returned" : error);

    error.clear();
    const auto contiguousError = memoryReader.PrepareContiguousRange(
        3u,
        2u,
        contiguousBytes,
        &error);
    Require(
        result,
        contiguousError == ContiguousViewStatus::Error && !error.empty(),
        "byteRange.contiguous.error",
        "invalid contiguous range did not produce an error status");

    error.clear();
    const auto contiguousUnavailable = reader.PrepareContiguousRange(
        0u,
        4u,
        contiguousBytes,
        &error);
    Require(
        result,
        contiguousUnavailable == ContiguousViewStatus::Unavailable && error.empty(),
        "byteRange.contiguous.unavailable",
        "reader without contiguous capability did not report unavailable");

    const auto prefetchUnavailable = memoryReader.PrefetchRange(0u, 4u);
    Require(
        result,
        prefetchUnavailable.IsUnavailable(),
        "byteRange.prefetch.unavailable",
        "reader without prefetch capability did not report unavailable");

    auto sharedMemoryReader = std::make_shared<MemoryByteRangeReader>(
        memoryOwner);
    SubrangeByteRangeReader subrangeReader(sharedMemoryReader, 1u, 2u);
    const auto prefetchError = subrangeReader.PrefetchRange(2u, 1u);
    Require(
        result,
        prefetchError.IsError() && !prefetchError.error.empty(),
        "byteRange.prefetch.error",
        "invalid prefetch range did not produce an error status");
    {
        std::vector<std::uint8_t> target(2u * kIoWindowBytes + 7u);
        std::array<std::size_t, 3u> sizes{};
        std::array<std::uint64_t, 3u> offsets{};
        std::size_t calls = 0u;
        CallbackByteRangeReader bounded(target.size() + 23u,
            [&](std::uint64_t offset, std::span<std::uint8_t> output, std::string*) {
                if (calls >= sizes.size()) { return false; }
                sizes[calls] = output.size();
                offsets[calls] = offset;
                ++calls;
                std::fill(output.begin(), output.end(), static_cast<std::uint8_t>(calls));
                return true;
            });
        Require(result, bounded.ReadAtCancellable(23u, target, {}) && calls == 3u &&
            sizes == std::array<std::size_t, 3u>{kIoWindowBytes, kIoWindowBytes, 7u} &&
            offsets == std::array<std::uint64_t, 3u>{23u, 23u + kIoWindowBytes, 23u + 2u * kIoWindowBytes} &&
            target.front() == 1u && target[kIoWindowBytes] == 2u && target.back() == 3u,
            "byteRange.windows", "large reads must sequentially fill exact one-MiB windows and the final tail");
        std::stop_source cancellation;
        calls = 0u;
        CallbackByteRangeReader cancellable(target.size(),
            [&](std::uint64_t, std::span<std::uint8_t>, std::string*) {
                ++calls;
                cancellation.request_stop();
                return true;
            });
        Require(result, !cancellable.ReadAtCancellable(0u, target, cancellation.get_token()) && calls == 1u,
            "byteRange.cancel-between-windows", "cancellation must prevent the next range read after current I/O returns");
        calls = 0u;
        Require(result, !cancellable.ReadAtCancellable(0u, target, cancellation.get_token()) && calls == 0u,
            "byteRange.cancel-before-window", "a stopped read must not invoke the source");
        CallbackByteRangeReader failing(target.size(),
            [&](std::uint64_t, std::span<std::uint8_t>, std::string*) { ++calls; return false; });
        Require(result, !failing.ReadAtCancellable(0u, target, {}) && calls == 1u,
            "byteRange.window-error", "a failed read must end the operation without replaying or starting another range");
    }
    const auto cancellationResult = RunDataCodecFeatureInputCancellation();
    result.passed &= cancellationResult.passed;
    result.failures.insert(result.failures.end(), cancellationResult.failures.begin(), cancellationResult.failures.end());
    result.AppendDiagnostics(cancellationResult.diagnostics);
    return result;
}

} // namespace datacodec::test

#endif
