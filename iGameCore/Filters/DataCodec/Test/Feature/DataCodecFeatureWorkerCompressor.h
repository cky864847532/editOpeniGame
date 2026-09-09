#ifndef DATACODEC_TEST_FEATURE_DATACODECFEATUREWORKERCOMPRESSOR_H
#define DATACODEC_TEST_FEATURE_DATACODECFEATUREWORKERCOMPRESSOR_H

#include "DataCodec/Codec/NumericArray/NumericArrayCodec.h"
#include "DataCodec/Runtime/Execution/ParallelExecution.h"
#include "DataCodec/Test/Common/DataCodecTestResult.h"

#include <array>
#include <cmath>

namespace datacodec::test {

inline TestResult RunDataCodecFeatureWorkerCompressor() {
    TestResult result;
    // 同时覆盖工作线程正常退出和无线程根的上下文释放
    for (const bool threaded : {false, true}) {
        for (std::size_t repeat = 0u; repeat < 3u; ++repeat) {
            DataCodecExecutionResources root(ResolvedResourceConfiguration{
                {1024u * 1024u, 1u, 1u}, 1024u * 1024u, 1u, threaded, true});
            CodecRunScope scope(root);
            auto phase = WaitForHeavyPhase(root);
            bool typedOptions = true;
            bool roundTrip = true;
            std::string error;
            const bool completed = phase && RunTerminalWork(root, *phase, [&](WorkerContext& worker) {
                std::array<double, 256u> input{};
                std::array<double, 256u> output{};
                for (std::size_t i = 0u; i < input.size(); ++i) {
                    input[i] = std::sin(static_cast<double>(i) * 0.1);
                }
                const numericarray::NumericArrayBufferLayout layout{
                    numericarray::MakeNumericArrayLayout(DataType::Float64, sizeof(double), input.size(), 1u), {}};
                auto& state = worker.NumericCompressor();
                for (const double tolerance : {0.001, 0.01, 0.001}) {
                    CompressorConfig config;
                    config.options["pressio:abs"] = tolerance;
                    config.options["pressio:nthreads"] = 8.0;
                    config.options["sz3:openmp"] = 1.0;
                    auto* compressor = state.Resolve(config, &error);
                    if (!compressor) { return false; }
                    numericarray::detail::numericarraycodec::OptionsHandle options(
                        pressio_compressor_get_options(compressor));
                    if (!options) { return false; }
                    std::uint32_t threads = 0u;
                    bool openmp = true;
                    typedOptions &= pressio_options_get_uinteger(options.get(), "pressio:nthreads", &threads)
                        == pressio_options_key_set && threads == 1u;
                    typedOptions &= pressio_options_get_bool(options.get(), "sz3:openmp", &openmp)
                        == pressio_options_key_set && !openmp;
                    numericarray::NumericArrayEncodedBytes encoded;
                    if (!numericarray::NumericArrayEncode::Compress(
                            {input.data(), layout}, config, encoded, &error, &state) ||
                        !numericarray::NumericArrayDecode::Decompress(encoded.Bytes(), layout, config,
                            {output.data(), layout}, &error, &state)) { return false; }
                    for (std::size_t i = 0u; i < input.size(); ++i) {
                        roundTrip &= std::abs(input[i] - output[i]) <= tolerance * 1.01;
                    }
                }
                return !worker.StopToken().stop_requested();
            });
            Require(result, completed && roundTrip, "worker.compressor-round-trip",
                "worker-owned compressor must support alternating configurations and encode/decode reuse");
            Require(result, typedOptions, "worker.compressor-serial-options",
                "double configuration entries must not enable library parallelism");
            phase.reset();
        }
    }
    return result;
}

} // 测试命名空间

#endif
