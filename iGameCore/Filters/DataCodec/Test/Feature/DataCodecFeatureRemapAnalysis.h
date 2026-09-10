#ifndef DATACODEC_TEST_FEATURE_DATACODECFEATUREREMAPANALYSIS_H
#define DATACODEC_TEST_FEATURE_DATACODECFEATUREREMAPANALYSIS_H

#include "DataCodec/Log/Analysis/AdapterPrecisionMetrics.h"
#include "DataCodec/Runtime/Record/RunRecordEmitter.h"
#include "DataCodec/Test/Common/DataCodecTestResult.h"

namespace datacodec::test {

inline TestResult RunDataCodecFeatureRemapAnalysis() {
    TestResult result;
    constexpr std::size_t count = kIoWindowBytes / sizeof(IndexType) + 3u;
    class ObservedProvider final : public IRemapProvider {
    public:
        explicit ObservedProvider(std::shared_ptr<const IRemapProvider> source) : source(std::move(source)) {}
        std::size_t Size() const noexcept override { return source->Size(); }
        bool IsIdentity() const noexcept override { return false; }
        std::uint64_t ResidentSizeHint() const noexcept override { return source->ResidentSizeHint(); }
        bool ReadAt(std::size_t index, IndexType& output, std::string* error) const override {
            return source->ReadAt(index, output, error);
        }
        bool ReadRange(std::uint64_t offset, std::span<IndexType> output, std::string* error) const override {
            ++reads;
            bounded &= output.size_bytes() <= kIoWindowBytes;
            if (failRead && reads == failRead) {
                return validation::AssignError(error, "injected analysis provider read failure");
            }
            return source->ReadRange(offset, output, error);
        }
        bool ReadRange(std::uint64_t, std::uint64_t, std::vector<IndexType>&, std::string* error) const override {
            bounded = false;
            return validation::AssignError(error, "analysis must use the bounded span overload");
        }
        std::shared_ptr<const IRemapProvider> source;
        mutable std::size_t reads{0u};
        mutable bool bounded{true};
        std::size_t failRead{0u};
    };
    for (const bool file : {false, true}) {
        const std::uint64_t capacity = file ? 0u : count * sizeof(IndexType);
        std::shared_ptr<resource::ResidentByteBudget> budget;
        std::shared_ptr<ObservedProvider> observed;
        log::RemapOrderSnapshot snapshot;
        {
            DataCodecExecutionResources root({{capacity, 1u, 1u}, capacity, 1u, false, true, file});
            CodecRunScope request(root);
            budget = root.StorageCapacity();
            bytestore::ByteStoreSession session;
            session.BindRun(root);
            auto writer = MakeStoreBackedWritableRemapProvider(count, session, false, "analysis_owner");
            if (!writer) { Require(result, false, "analysis.owner-create", "remap owner is required"); continue; }
            std::vector<IndexType> order(count);
            for (std::size_t i = 0u; i < count; ++i) { order[i] = static_cast<IndexType>(count - i - 1u); }
            const bool ready = writer->AppendRange(order) && writer->EndWrite();
            observed = std::make_shared<ObservedProvider>(writer);
            log::RemapOrderCapture capture;
            RunRecordEmitter emitter;
            emitter.Reset({}, &capture);
            emitter.RecordRemapOrder("leaf", RunRemapDomain::Point, observed);
            snapshot = capture.TakeSnapshot();
            writer.reset();
            session.ReleaseAll();
            Require(result, ready && observed->reads == 0u && snapshot.pointOrders.at("leaf") == observed &&
                budget->Snapshot().reservedBytes == capacity,
                "analysis.capture-owner", "capture must share the existing owner without reading or allocating a full order");
        }
        struct Values { std::size_t count; bool reverse; } original{count, false}, reversed{count, true};
        const auto view = [](const Values& values) {
            NumericArrayView array;
            array.scalarType = ScalarType::Float64;
            array.layout = ArrayLayout::GetterOnly;
            array.origin = ViewBufferOrigin::Borrowed;
            array.tupleCount = values.count;
            array.componentCount = 1u;
            array.userData = &values;
            array.getTupleBytes = [](const void* data, std::size_t index, void* output, std::string*) {
                const auto& values = *static_cast<const Values*>(data);
                const double value = static_cast<double>(values.reverse ? values.count - index - 1u : index);
                std::memcpy(output, &value, sizeof(value));
                return true;
            };
            return array;
        };
        log::LogAnalysisResult status;
        auto metric = log::ComputeNumericFieldPrecision("leaf", log::NumericFieldPrecisionKind::Geometry,
            "values", view(original), view(reversed), status, "analysis", snapshot.pointOrders.at("leaf").get());
        Require(result, metric.ok && status.passed && metric.maxAbsError == 0.0 &&
            observed->bounded && observed->reads == 2u,
            "analysis.windowed-precision", "analysis must read the surviving owner in one full window and a tail");
        observed->reads = 0u;
        log::NumericFieldSignature signature;
        log::LogAnalysisResult signatureStatus;
        Require(result, log::BuildNumericFieldSignature("values", view(original), {.maxTuplesPerField = count},
            signature, signatureStatus, "analysis", observed.get()) && signatureStatus.passed &&
            observed->bounded && observed->reads == 2u && signature.sampledTupleCount == count &&
            signature.sums == std::vector<double>{static_cast<double>(count) * (count - 1u) / 2.0} &&
            signature.minimums == std::vector<double>{0.0} && signature.maximums == std::vector<double>{count - 1.0} &&
            signature.sums.capacity() == 1u && signature.minimums.capacity() == 1u && signature.maximums.capacity() == 1u,
            "analysis.signature-window-and-summary", "signature must traverse fixed remap windows and keep only per-component summary arrays");
        observed->reads = 0u;
        observed->failRead = 2u;
        log::LogAnalysisResult failure;
        Require(result, !log::BuildNumericFieldSignature("values", view(original),
            {.maxTuplesPerField = count}, signature, failure, "analysis", observed.get()) && !failure.passed,
            "analysis.read-failure", "failed remap reads must report analysis failure");
        observed.reset();
        snapshot = {};
        Require(result, budget->Snapshot().reservedBytes == 0u,
            "analysis.last-owner", "analysis completion must release the original capacity lease");
    }
    return result;
}

} // 测试命名空间

#endif
