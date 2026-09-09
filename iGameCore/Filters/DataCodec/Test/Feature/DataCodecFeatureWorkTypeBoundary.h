#ifndef DATACODEC_TEST_FEATURE_DATACODECFEATUREWORKTYPEBOUNDARY_H
#define DATACODEC_TEST_FEATURE_DATACODECFEATUREWORKTYPEBOUNDARY_H

#include "DataCodec/Runtime/Execution/ParallelExecution.h"
#include "DataCodec/Codec/NumericArray/NumericArrayBlockReader.h"
#include "DataCodec/Test/Common/DataCodecTestResult.h"
#include <atomic>

namespace datacodec::test {

inline TestResult RunDataCodecFeatureWorkTypeBoundary() {
    TestResult result;
    for (const bool threaded : {false, true}) {
        const std::size_t concurrency = threaded ? 2u : 1u;
        const std::size_t slots = threaded ? 4u : 1u;
        DataCodecExecutionResources run(ResolvedResourceConfiguration{
            {64u, concurrency, slots}, 64u, threaded ? 4u : 1u, threaded, true});
        run.BeginRun();
        std::atomic_size_t retired{0u};
        std::size_t next = 0u, committed = 0u, descriptions = 0u;
        bool boundary = true;
        struct Input {
            std::size_t ordinal{0u};
            std::atomic_size_t* retired{nullptr};
            ~Input() { if (retired) { retired->fetch_add(1u); } }
        };
        struct Output { std::size_t ordinal{0u}; };
        const std::array<std::uint32_t, 5u> modes{0u, 0u, 1u, 1u, 0u};
        const bool success = RunOrderedBlocks<Input, Output>(run,
            [&] { return next < modes.size(); },
            [&](Input& input) {
                if (next == 2u || next == 4u) {
                    boundary &= committed == next && retired.load() == next;
                }
                input.ordinal = next++;
                input.retired = &retired;
                return true;
            },
            [&](const Input& input, Output& output, WorkerContext&) {
                output.ordinal = input.ordinal;
                return true;
            },
            [&](Output& output) { return output.ordinal == committed++; }, false,
            [&] {
                ++descriptions;
                return ResourceWorkType{.path = ResourceWorkPath::AttributeDecode,
                    .codec = 1u, .scalar = 1u, .components = 3u,
                    .blockElements = numericarray::kSpatialBlockElementCount,
                    .referencePath = modes[next]};
            });
        Require(result, success && boundary && committed == modes.size() && retired.load() == modes.size() &&
            descriptions == modes.size() && run.Concurrency() == concurrency,
            "work-type.drained-boundary", "each type boundary must retire prior inputs before reading the next type while preserving Fixed limits");
        run.EndRun();
    }
    {
        DataCodecExecutionResources run(ResolvedResourceConfiguration{{64u, 1u, 1u}, 64u, 1u, false, true});
        CacheResources resources;
        resources.BindRun(run);
        numericarray::NumericArrayBlockParams params;
        params.dataType = DataType::Float32;
        params.componentCount = 3u;
        std::vector<NumericArrayBlockLayoutParams> layouts(3u);
        layouts[0].elementCount = numericarray::kSpatialBlockElementCount;
        layouts[0].bytesCodec = NumericArrayBytesCodec::NumericArrayCodec;
        layouts[1] = layouts[0];
        layouts[1].elementCount = 1u;
        layouts[1].elementOffset = layouts[0].elementCount;
        layouts[2] = layouts[0];
        layouts[2].mode = NumericArrayBlockMode::WaveletReference;
        layouts[2].referenceKind = NumericArrayReferenceKind::TemporalKeyFrame;
        layouts[2].elementCount = numericarray::kSpatialBlockElementCount * 4u;
        layouts[2].componentLayouts.push_back({.bytesCodec = NumericArrayBytesCodec::IntegerDeltaRunVarint});
        int unusedStream = 0;
        numericarray::NumericDecodeCursor<int> cursor{unusedStream, params, layouts, resources};
        const auto ordinary = cursor.NextWorkType(ResourceWorkPath::AttributeDecode);
        cursor.nextBlock = 1u;
        const auto tail = cursor.NextWorkType(ResourceWorkPath::AttributeDecode);
        cursor.nextBlock = 2u;
        const auto reference = cursor.NextWorkType(ResourceWorkPath::AttributeDecode);
        Require(result, ordinary == tail && ordinary != reference &&
            reference.blockElements == 4u * numericarray::kSpatialBlockElementCount &&
            reference.componentCodecMask == 6u && reference.libraryThreads == 1u,
            "work-type.metadata-key", "tail blocks must preserve the type while old granularity and actual reference/codec paths distinguish it");
    }
    return result;
}

}

#endif
