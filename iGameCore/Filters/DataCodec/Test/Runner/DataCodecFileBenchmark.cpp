#include <CGNS/iGameCGNSReader.h>
#include "DataCodec/API/Entry/DataCodecEncodeEntry.h"
#include "DataCodec/API/Entry/DecodeStorageAnalysis.h"
#include "DataCodec/Workflow/Session/CodecRunEntry.h"
#include "DataCodec/Workflow/Session/DecodeSession.h"
#include "DataCodec/Filter/Adapter/iGameDecodeAdapter.h"
#include "DataCodec/Filter/Adapter/iGameEncodeAdapter.h"
#include "DataCodec/Filter/Adapter/iGameFramePackageDecodeAssembly.h"
#include "DataCodec/Filter/Adapter/iGameDataCodecAttributeCatalog.h"
#include "DataCodec/Filter/Adapter/iGameDataCodecDataObjectBridge.h"
#include "DataCodec/Storage/ByteIO/FileByteRangeIO.h"
#include <windows.h>
#include <psapi.h>
#include <chrono>
#include <array>
#include <charconv>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <thread>
#include <cmath>
#include "DataCodec/Test/Experiment/DataCodecMemoryInvestigation.h"
#include "DataCodec/Test/Experiment/DataCodecBlockResponseInvestigation.h"

namespace {
using Clock = std::chrono::steady_clock;
double Seconds(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}
double CpuSeconds() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) { return -1.0; }
    const auto ticks = [](const FILETIME value) {
        return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32u) | value.dwLowDateTime;
    };
    return static_cast<double>(ticks(kernel) + ticks(user)) / 10000000.0;
}
void Memory(const char* phase) {
    PROCESS_MEMORY_COUNTERS_EX process{};
    MEMORYSTATUSEX system{};
    system.dwLength = sizeof(system);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process), sizeof(process)) &&
        GlobalMemoryStatusEx(&system)) {
        std::cout << "MEMORY phase=" << phase << " working_set=" << process.WorkingSetSize
                  << " process_lifetime_peak_working_set=" << process.PeakWorkingSetSize
                  << " private_bytes=" << process.PrivateUsage << " available_physical=" << system.ullAvailPhys
                  << " total_physical=" << system.ullTotalPhys << std::endl;
    }
}
class Records final : public datacodec::IRunRecordSink {
public:
    std::function<void(std::string_view)> investigationMessageHook;
    datacodec::RunRecordMask Interests() const noexcept override {
        return datacodec::RunRecordKind::RunEnd | datacodec::RunRecordKind::Message |
            datacodec::RunRecordKind::StageTiming | datacodec::RunRecordKind::Progress;
    }
    void Submit(const datacodec::RunRecord& record) override {
        std::lock_guard lock(m_mutex);
        if (const auto* r = std::get_if<datacodec::RunEndRecord>(&record)) {
            std::cout << "RUN_END id=" << r->run.runId << " success=" << r->success
                      << " elapsed_ms=" << r->elapsedMs << " source_bytes=" << r->sourceBytes
                      << " input_bytes=" << r->inputBytes << " output_bytes=" << r->outputBytes << std::endl;
        } else if (const auto* r = std::get_if<datacodec::RunMessageRecord>(&record)) {
            std::cout << "MESSAGE " << r->message.code << ' ' << r->message.text << ' '
                      << r->message.technicalDetail << std::endl;
            if (investigationMessageHook) { investigationMessageHook(r->message.text); }
        } else if (const auto* r = std::get_if<datacodec::RunStageTimingRecord>(&record)) {
            std::cout << "STAGE " << r->stage.name << " elapsed_ms=" << r->stage.elapsedMs
                      << " scope=" << r->stage.scope << std::endl;
        } else if (const auto* r = std::get_if<datacodec::RunProgressRecord>(&record)) {
            if (Seconds(m_lastProgress) >= 10.0) {
                m_lastProgress = Clock::now();
                std::cout << "PROGRESS " << r->normalized << ' ' << r->text << ' ' << r->technicalDetail << std::endl;
            }
        }
    }
private:
    std::mutex m_mutex;
    Clock::time_point m_lastProgress{};
};
struct Shape {
    std::size_t leaves{}, points{}, cells{}, attributes{}, attributeValues{};
    bool operator==(const Shape&) const = default;
};
Shape Describe(iGame::DataObject::Pointer object, const char* phase) {
    iGame::iGameBlockTreeAdapter tree(object);
    Shape shape;
    for (const auto& leaf : tree.GetLeaves()) {
        const auto adapter = tree.GetLeaf(leaf.path);
        ++shape.leaves;
        shape.points += adapter->GetNumberOfPoints();
        shape.cells += adapter->GetNumberOfCells();
        const auto attrs = adapter->GetNumberOfPointAttrs() + adapter->GetNumberOfCellAttrs();
        shape.attributes += attrs;
        for (std::size_t i = 0; i < attrs; ++i) {
            const auto& attr = i < adapter->GetNumberOfPointAttrs() ? adapter->GetPointAttr(i) :
                adapter->GetCellAttr(i - adapter->GetNumberOfPointAttrs());
            shape.attributeValues += attr.GetElementCount() * attr.GetComponentCount();
            std::cout << "ATTRIBUTE phase=" << phase << " leaf=" << leaf.path << " name=" << attr.GetName()
                      << " type=" << static_cast<int>(attr.GetDataType()) << " tuples=" << attr.GetElementCount()
                      << " components=" << attr.GetComponentCount() << std::endl;
        }
    }
    std::cout << "SHAPE phase=" << phase << " leaves=" << shape.leaves << " points=" << shape.points
              << " cells=" << shape.cells << " attributes=" << shape.attributes
              << " attribute_values=" << shape.attributeValues << std::endl;
    return shape;
}

// 复用桌面适配层的内部解码路径，显式持有根对象以读取状态
iGame::DataCodecDataObjectDecodeResult DecodeWithMemoryTrace(
    std::shared_ptr<datacodec::IByteRangeReader> input,
    const datacodec::CodecResourceParams& resources, std::shared_ptr<Records> records,
    bool pulseFinalGate = false, bool responseInvestigation = false, bool probeBlock = false,
    bool* probeCompleted = nullptr) {
    auto resolved = datacodec::ResolveResourceConfiguration(resources, datacodec::ProbeResources());
    if (responseInvestigation && resources.maxComputeThreads == 1u) {
        resolved.threaded = false;
        resolved.initialLimits.slotLimit = 1u;
    }
    datacodec::DataCodecExecutionResources root(resolved);
    datacodec::CodecRunScope scope(root);
    if (!scope) { throw std::runtime_error("investigation decode could not begin"); }
    datacodec::DecodeSession session;
    iGame::iGameDecodeAdapter adapter;
    iGame::iGameFramePackageDecodeAssembly assembly;
    std::jthread gateReopener;
    bool gatePulsed = false;
    bool publishedWithClosedGate = false;
    // 调查专用，最终提交处只关闭一秒准入门，不改变字节额度或 CPU 参数
    if (pulseFinalGate) {
        records->investigationMessageHook = [&](std::string_view message) {
            if (gatePulsed && message.starts_with("point=commit.attributes.end;")) {
                datacodec::ResourceDebugSnapshot snapshot;
                publishedWithClosedGate = root.TryCopyResourceDebugSnapshot(snapshot) && !snapshot.gateOpen;
                return;
            }
            if (gatePulsed || !message.starts_with("point=commit.attributes.begin;")) { return; }
            datacodec::ResourceDebugSnapshot snapshot;
            if (!root.TryCopyResourceDebugSnapshot(snapshot)) { return; }
            if (!root.UpdateLimits(snapshot.limits, false, datacodec::ResourceDecisionReason::MechanismCheck)) { return; }
            gatePulsed = true;
            gateReopener = std::jthread([&, limits = snapshot.limits] {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                root.UpdateLimits(limits, true, datacodec::ResourceDecisionReason::MechanismCheck);
            });
        };
    }
    std::unique_ptr<datacodec::test::BlockResponseInvestigationRun> response;
    if (responseInvestigation) {
        response = std::make_unique<datacodec::test::BlockResponseInvestigationRun>(root, *records, probeBlock);
        records->investigationMessageHook = [&](std::string_view message) { response->StageMessage(message); };
    }
    struct ClearInvestigationHook {
        Records& records;
        ~ClearInvestigationHook() { records.investigationMessageHook = {}; }
    } clearHook{*records};
    std::unique_ptr<datacodec::test::MemoryInvestigationTrace> trace;
    if (!responseInvestigation) { trace = std::make_unique<datacodec::test::MemoryInvestigationTrace>(root, "decode"); }
    auto result = datacodec::DecodePackageInRun({.inputReader = std::move(input),
        .leafAdapter = &adapter, .frameAssembly = &assembly,
        .attributeSelection = datacodec::AttributeSelectionMode::AllAvailable,
        .runRecordSink = records}, root, &session);
    if (result.failure) { root.RecordFailure(*result.failure); }
    iGame::DataCodecDataObjectDecodeResult decoded;
    decoded.success = scope.Finish(result.success);
    if (pulseFinalGate) {
        decoded.success &= gatePulsed && publishedWithClosedGate;
        std::cout << "COMMIT_GATE pulsed=" << gatePulsed
                  << " published_while_closed=" << publishedWithClosedGate << std::endl;
    }
    decoded.output = result.decodedFramePackage ? assembly.Output() : adapter.TakeDataObject();
    if (root.FirstFailure()) { std::cerr << datacodec::FormatCodecFailure(*root.FirstFailure()) << '\n'; }
    if (trace) { trace->Finish(); }
    if (response) {
        if (probeCompleted) { *probeCompleted = response->ProbeCompleted(); }
        response->Finish();
    }
    return decoded;
}

// 用真实宿主适配器核验直接发布、混合回放和取消，逐值检查输出
int InvestigateAttributeCommit(Records& records) {
    using namespace datacodec;
    using namespace std::chrono_literals;
    constexpr std::uint64_t MiB = 1024u * 1024u;
    const std::array<double, 2u> values{3.25, -7.5};
    int failures = 0;
    const auto record = [&](const char* name, bool ok) {
        failures += !ok;
        RunMessageRecord message;
        message.message.code = "scheduler-investigation";
        message.message.text = std::string("case=") + name + ";passed=" + (ok ? "1" : "0");
        records.TrySubmit(message);
    };
    for (const auto scenario : {0, 1, 2, 3}) {
        const bool mixed = scenario == 1, cancelled = scenario == 2, incomplete = scenario == 3;
        RuntimeResourceLimits limits{MiB, 1u, 2u};
        DataCodecExecutionResources root(ResolvedResourceConfiguration{limits, MiB, 1u, true, true});
        CodecRunScope scope(root);
        CacheResources runtime;
        runtime.BindRun(root);
        bytestore::ByteStoreSession stores;
        stores.BindStorage(root.StorageCapacity(), false);
        CodecStorageParams params;
        params.attrParams.resize(mixed ? 2u : 1u);
        for (std::size_t i = 0u; i < params.attrParams.size(); ++i) {
            auto& meta = params.attrParams[i];
            meta.name = "commit-check-" + std::to_string(i);
            meta.type = AttrRole::Scalar;
            meta.attachmentType = AttrAttachment::Point;
            meta.dataType = DataType::Float64;
            meta.dimension = 1;
            meta.elementCount = values.size();
        }
        iGame::iGameDecodeAdapter adapter;
        DecodedAttributeCacheSet attributes;
        if (!scope || !adapter.SetMeshType(MeshType::PointSet) || !attributes.Initialize(params, stores)) { return 6; }
        auto native = adapter.CreateAttributeDecodeStore(0u, params.attrParams[0]);
        if (!native || !attributes.BindAttributeStore(0u, native, true)) { return 6; }
        std::vector<std::size_t> indices;
        for (std::size_t i = 0u; i < params.attrParams.size(); ++i) {
            if (!attributes.BeginAttribute(i, params.attrParams[i]) ||
                !attributes.WriteAttributeRange(i, 0u, values.size(), values.data(), sizeof(values)) ||
                (!incomplete && !attributes.EndAttribute(i))) { return 6; }
            indices.push_back(i);
        }
        // 直接发布仍需由活动请求的 driver 调用
        if (scenario == 0) {
            bool rejected = false;
            std::jthread other([&] {
                std::string error;
                rejected = !CommitAttributeCacheFields(adapter, runtime, attributes, indices, &error) && !error.empty();
            });
            other.join();
            record("attribute_publish_requires_driver", rejected);
        }
        if (!root.UpdateLimits(limits, false, ResourceDecisionReason::MechanismCheck)) { return 6; }
        if (cancelled) { root.RequestStop(); }
        bool observedReplayWait = false;
        // 有界看门线程使旧行为也能返回测试结果，混合路径在观察到等待后开门
        std::jthread reopen([&](std::stop_token stop) {
            const auto deadline = Clock::now() + 500ms;
            while (!stop.stop_requested() && Clock::now() < deadline) {
                ResourceDebugSnapshot s;
                if (mixed && root.TryCopyResourceDebugSnapshot(s) && !s.gateOpen &&
                    s.waiting == ResourceWaitReason::PressureRecovery && !s.heavyPhaseAdmitted) {
                    observedReplayWait = true;
                    root.UpdateLimits(limits, true, ResourceDecisionReason::MechanismCheck);
                    return;
                }
                std::this_thread::sleep_for(1ms);
            }
            if (!stop.stop_requested()) { root.UpdateLimits(limits, true, ResourceDecisionReason::MechanismCheck); }
        });
        std::string error;
        const bool committed = CommitAttributeCacheFields(adapter, runtime, attributes, indices, &error);
        ResourceDebugSnapshot after;
        const bool closed = root.TryCopyResourceDebugSnapshot(after) && !after.gateOpen;
        reopen.request_stop();
        reopen.join();
        auto object = adapter.TakeDataObject();
        iGame::iGameEncodeAdapter output(object);
        const auto expectedCount = cancelled || incomplete ? 0u : params.attrParams.size();
        bool contents = output.GetNumberOfPointAttrs() == expectedCount;
        for (std::size_t i = 0u; contents && i < expectedCount; ++i) {
            const auto& attribute = output.GetPointAttr(i);
            for (std::size_t j = 0u; j < values.size(); ++j) {
                double value = 0.0;
                attribute.GetTuple(j, &value);
                contents &= value == values[j];
            }
        }
        attributes.Reset();
        native.reset();
        stores.ReleaseAll();
        const bool finished = scope.Finish(committed);
        if (scenario == 0) { record("attribute_publish_completes_with_closed_gate", committed && closed && contents && finished); }
        if (mixed) { record("mixed_attribute_replay_waits_and_preserves_values", committed && observedReplayWait && contents && finished); }
        if (cancelled) { record("cancel_prevents_attribute_publish", !committed && closed && contents && !finished); }
        if (incomplete) { record("incomplete_attribute_is_not_published", !committed && closed && contents && !error.empty() && !finished); }
    }
    return failures ? 6 : 0;
}
}

int main(int argc, char** argv) {
    try {
        if ((argc == 6 || argc == 7) && std::string_view(argv[1]) == "--investigate-response") {
            const std::string_view mode(argv[3]);
            const bool stream = argc == 7 && std::string_view(argv[6]) == "stream";
            if (argc == 7 && !stream) { return 2; }
            if (mode != "fixed" && mode != "adaptive" && mode != "probe") { return 2; }
            double value{};
            std::size_t workers{};
            const std::string_view parameter(argv[4]), threadCount(argv[5]);
            const auto p = std::from_chars(parameter.data(), parameter.data() + parameter.size(), value);
            const auto t = std::from_chars(threadCount.data(), threadCount.data() + threadCount.size(), workers);
            if (p.ec != std::errc{} || p.ptr != parameter.data() + parameter.size() ||
                t.ec != std::errc{} || t.ptr != threadCount.data() + threadCount.size() ||
                !std::isfinite(value) || value <= 0.0 || value >= (mode == "fixed" ? 1048576.0 : 100.0) ||
                workers < 1u || workers > 16u || (mode == "probe" && workers != 1u)) { return 2; }
            datacodec::CodecResourceParams resources;
            resources.maxComputeThreads = workers;
            if (mode == "fixed") {
                resources.mode = datacodec::CodecResourceMode::Fixed;
                resources.ownedStorageLimitBytes = static_cast<std::uint64_t>(value * 1024.0 * 1024.0);
            } else { resources.targetAvailableMemoryRatio = value / 100.0; }
            std::cout << std::fixed << std::setprecision(3);
            std::cout << "RESPONSE_CONFIG mode=" << mode << " value=" << value << " workers=" << workers
                      << " reader=" << (stream ? "stream" : "bounded-file") << std::endl;
            Memory("before_decode");
            bool probeCompleted = false;
            std::shared_ptr<datacodec::IByteRangeReader> reader;
            if (stream) { reader = std::make_shared<datacodec::test::ResponseStreamReader>(argv[2]); }
            else { reader = std::make_shared<::datacodec::FileByteRangeReader>(argv[2]); }
            const auto decoded = DecodeWithMemoryTrace(std::move(reader),
                resources, std::make_shared<Records>(), false, true, mode == "probe", &probeCompleted);
            std::cout << "RESULT decode_success=" << decoded.success << " probe_completed=" << probeCompleted << std::endl;
            Memory("after_decode");
            if (decoded.success && decoded.output) { Describe(decoded.output, "decoded"); }
            return mode == "probe" ? (probeCompleted ? 0 : 6) : (decoded.success ? 0 : 5);
        }
        if (argc == 2 && std::string_view(argv[1]) == "--investigate-memory-controller") {
            return datacodec::test::InvestigateMemoryController();
        }
        if (argc == 2 && std::string_view(argv[1]) == "--investigate-scheduler") {
            Records records;
            const auto scheduler = datacodec::test::InvestigateScheduler(records);
            const auto commit = InvestigateAttributeCommit(records);
            return scheduler || commit ? 6 : 0;
        }
        if (argc == 4 && (std::string_view(argv[1]) == "--investigate-decode" ||
            std::string_view(argv[1]) == "--investigate-decode-fixed" ||
            std::string_view(argv[1]) == "--investigate-decode-gated")) {
            const bool pulseGate = std::string_view(argv[1]) == "--investigate-decode-gated";
            const bool fixed = std::string_view(argv[1]) != "--investigate-decode";
            if (pulseGate && std::getenv("IGAME_DATACODEC_SCHEDULER_TRACE") == nullptr) { return 2; }
            double percent{};
            const std::string_view value(argv[3]);
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), percent);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                !std::isfinite(percent) || percent < 0.0 || percent >= (fixed ? 1048576.0 : 100.0)) { return 2; }
            datacodec::CodecResourceParams resources;
            resources.maxComputeThreads = 16u;
            if (fixed) {
                resources.mode = datacodec::CodecResourceMode::Fixed;
                resources.ownedStorageLimitBytes = static_cast<std::uint64_t>(percent * 1024.0 * 1024.0);
            } else { resources.targetAvailableMemoryRatio = percent / 100.0; }
            std::cout << std::fixed << std::setprecision(3);
            Memory("before_decode");
            const auto started = Clock::now();
            const auto decoded = DecodeWithMemoryTrace(std::make_shared<::datacodec::FileByteRangeReader>(argv[2]),
                resources, std::make_shared<Records>(), pulseGate);
            std::cout << "RESULT decode_success=" << decoded.success << " decode_seconds=" << Seconds(started) << std::endl;
            Memory("after_decode");
            if (decoded.success && decoded.output) { Describe(decoded.output, "decoded"); return 0; }
            return 5;
        }
        if ((argc == 3 || argc == 4 || argc == 5) && std::string_view(argv[1]) == "--storage-bound") {
            std::int64_t deltaMiB = 0;
            if (argc >= 4) {
                const std::string_view value(argv[3]);
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), deltaMiB);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                    deltaMiB < -1048576 || deltaMiB > 1048576) { return 2; }
            }
            std::size_t workers = 1u;
            if (argc == 5) {
                const std::string_view value(argv[4]);
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), workers);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || workers == 0u || workers > 32u) { return 2; }
            }
            auto reader = std::make_shared<::datacodec::FileByteRangeReader>(argv[2]);
            const auto start = Clock::now();
            const auto analysis = datacodec::AnalyzeDecodeStorage({.inputReader = reader, .adapterBackedAttributes = true, .adapterBackedGeometry = true, .adapterBackedConnectivity = true});
            if (!analysis.success) {
                std::cerr << (analysis.failure ? datacodec::FormatCodecFailure(*analysis.failure) : "analysis cancelled") << '\n';
                return 5;
            }
            const auto floor = *analysis.minimumExecutionLimitBytes;
            const auto signedLimit = static_cast<std::int64_t>(floor) + deltaMiB * 1024 * 1024;
            if (signedLimit < 0) { return 2; }
            const auto limit = static_cast<std::uint64_t>(signedLimit);
            std::cout << "STORAGE_ANALYSIS bytes=" << floor << " seconds=" << Seconds(start)
                << " stage=" << analysis.peakStage << " frame=" << analysis.peakFrameIndex
                << " leaf=" << analysis.peakLeafPath << " leaves=" << analysis.inspectedLeafCount << std::endl;
            std::cout << "COUNT_SCAN milliseconds=" << analysis.polyhedronCountScanMilliseconds << std::endl;
            for (std::size_t i = 0; i < analysis.peakBytesByKind.size(); ++i) {
                std::cout << "STORAGE_PART kind=" << i << " bytes=" << analysis.peakBytesByKind[i] << std::endl;
            }
            datacodec::DataCodecExecutionResources root(datacodec::ResolvedResourceConfiguration{
                {limit, workers, workers == 1u ? 1u : workers + 1u}, limit, workers, workers != 1u, true, false});
            datacodec::CodecRunScope scope(root);
            datacodec::DecodeSession session;
            iGame::iGameDecodeAdapter adapter;
            iGame::iGameFramePackageDecodeAssembly assembly;
            Memory("before_bounded_decode");
            const auto decodeStart = Clock::now();
            const auto decoded = datacodec::DecodePackageInRun({.inputReader = reader,
                .leafAdapter = &adapter, .frameAssembly = &assembly,
                .runRecordSink = std::make_shared<Records>()}, root, &session);
            const auto decodeSeconds = Seconds(decodeStart);
            const auto storage = root.StorageCapacity()->Snapshot();
            datacodec::ResourceDebugSnapshot flow;
            const auto snapshotDeadline = Clock::now() + std::chrono::seconds(2);
            while (!root.TryCopyResourceDebugSnapshot(flow)) {
                if (Clock::now() >= snapshotDeadline) {
                    std::cerr << "resource snapshot unavailable after bounded decode\n";
                    return 6;
                }
                std::this_thread::yield();
            }
            const auto scratch = root.Scratch().SnapshotStats();
            std::cout << "ADMISSION compute_limit=" << workers << " peak_slots=" << flow.peakAdmittedBlocks
                << " peak_compute=" << flow.peakActiveComputeUnits << " byte_wait_seconds="
                << std::chrono::duration<double>(flow.byteWaitDuration).count()
                << " reuse=" << scratch.reusedBlockCount << " allocations="
                << root.StorageCapacity()->AllocatedStorage().allocationCount << std::endl;
            std::cout << "STORAGE_RESULT success=" << decoded.success << " limit=" << limit
                << " planned=" << floor << " observed_peak=" << storage.peakReservedBytes
                << " reserved=" << storage.reservedBytes << " seconds=" << decodeSeconds << std::endl;
            Memory("after_bounded_decode");
            if (!decoded.success) {
                if (decoded.failure) { std::cerr << datacodec::FormatCodecFailure(*decoded.failure) << '\n'; }
                return 5;
            }
            Describe(decoded.decodedFramePackage ? assembly.Output() : adapter.TakeDataObject(), "bounded_decoded");
            return storage.peakReservedBytes <= limit ? 0 : 6;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--decode") {
            std::cout << std::fixed << std::setprecision(3);
            std::cout << "CONFIG decode_only=true mode=Adaptive threads=automatic storage=automatic all_attributes=true" << std::endl;
            Memory("before_decode");
            const auto start = Clock::now();
            iGame::DataCodecDataObjectDecodeRequest request;
            request.inputReader = std::make_shared<::datacodec::FileByteRangeReader>(argv[2]);
            request.loadAllAvailableAttributes = true;
            request.runRecordSink = std::make_shared<Records>();
            const auto decoded = iGame::DecodeDataCodecDataObject(request);
            std::cout << "RESULT decode_success=" << decoded.success << " decode_seconds=" << Seconds(start)
                      << " decoded_cache_hit=" << decoded.decodedFrameCacheHit << std::endl;
            Memory("after_decode");
            if (!decoded.success || decoded.output == nullptr) {
                for (const auto& m : decoded.messages) { std::cerr << m.code << ' ' << m.text << ' ' << m.technicalDetail << '\n'; }
                return 5;
            }
            Describe(decoded.output, "decoded");
            return 0;
        }
        if (argc < 3 || (argc - 3) % 2 != 0) {
            std::cerr << "Usage: iGameDataCodecFileBenchmark input.cgns new-output.igc "
                "[--memory unlimited|fixed|adaptive] [--threads N | --cpu-idle percent] [--memory-reserve percent]\n";
            return 2;
        }
        datacodec::CodecResourceParams resources;
        bool memoryTrace = false;
        for (int i = 3; i < argc; i += 2) {
            const std::string_view option(argv[i]), value(argv[i + 1]);
            if (option == "--memory-trace" && value == "on") {
                memoryTrace = true;
            } else if (option == "--memory") {
                if (value == "unlimited") { resources.mode = datacodec::CodecResourceMode::Unlimited; }
                else if (value == "fixed") { resources.mode = datacodec::CodecResourceMode::Fixed; }
                else if (value == "adaptive") { resources.mode = datacodec::CodecResourceMode::Adaptive; }
                else { std::cerr << "Invalid memory mode\n"; return 2; }
            } else if (option == "--threads") {
                std::size_t threads = 0u;
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), threads);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || threads == 0u) {
                    std::cerr << "Invalid fixed thread count\n"; return 2;
                }
                resources.threadMode = datacodec::CodecThreadMode::Fixed;
                resources.targetCpuIdleRatio.reset();
                resources.maxComputeThreads = threads;
            } else if (option == "--cpu-idle" || option == "--memory-reserve") {
                double percent = 0.0;
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), percent);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                    !std::isfinite(percent) || percent < 0.0 || percent >= 100.0) {
                    std::cerr << "Invalid resource percentage\n"; return 2;
                }
                if (option == "--cpu-idle") {
                    resources.threadMode = datacodec::CodecThreadMode::Adaptive;
                    resources.maxComputeThreads.reset();
                    resources.targetCpuIdleRatio = percent / 100.0;
                } else { resources.targetAvailableMemoryRatio = percent / 100.0; }
            } else {
                std::cerr << "Unknown resource option\n";
                return 2;
            }
        }
        const std::filesystem::path input(argv[1]), output(argv[2]);
        if (memoryTrace && resources.mode != datacodec::CodecResourceMode::Adaptive) {
            std::cerr << "Memory investigation requires Adaptive mode\n"; return 2;
        }
        if (!std::filesystem::is_regular_file(input) || std::filesystem::exists(output)) {
            std::cerr << "Input missing or output already exists\n";
            return 2;
        }
        std::cout << std::fixed << std::setprecision(3);
        std::cout << "CONFIG mode=" << datacodec::CodecResourceModeName(resources.mode)
                  << " thread_mode=" << datacodec::CodecThreadModeName(resources.threadMode)
                  << " threads=" << (resources.maxComputeThreads ? std::to_string(*resources.maxComputeThreads) : "automatic")
                  << " cpu_idle=" << resources.targetCpuIdleRatio.value_or(0.0)
                  << " memory_reserve=" << resources.targetAvailableMemoryRatio.value_or(0.20)
                  << " storage=automatic numeric=lossless all_attributes=true"
                  << " logical_processors=" << std::thread::hardware_concurrency()
                  << " source_file_bytes=" << std::filesystem::file_size(input) << std::endl;
        Memory("before_load");
        iGame::iGameCGNSReader::Pointer reader = iGame::iGameCGNSReader::New();
        auto start = Clock::now();
        std::cout << "BEGIN load" << std::endl;
        auto source = reader->ReadFile(input.string());
        const auto loadSeconds = Seconds(start);
        if (source == nullptr) { std::cerr << "CGNS load failed\n"; return 3; }
        std::cout << "RESULT load_seconds=" << loadSeconds << std::endl;
        Memory("after_load");
        const auto sourceShape = Describe(source, "source");
        auto records = std::make_shared<Records>();
        datacodec::EncodeResult encoded;
        const auto encodeCpuStart = CpuSeconds();
        start = Clock::now();
        std::cout << "BEGIN encode" << std::endl;
        {
            ::datacodec::FileByteRangeOutput sink(output);
            datacodec::EncodeRequest request;
            std::unique_ptr<iGame::iGameBlockTreeAdapter> tree;
            std::unique_ptr<iGame::iGameEncodeAdapter> leaf;
            const auto kind = iGame::ResolveDataCodecEncodePackageKind(source);
            if (kind == datacodec::EncodePackageKind::FramePackage) {
                tree = std::make_unique<iGame::iGameBlockTreeAdapter>(source);
                request.input = datacodec::EncodeInput::BlockTreeAdapter(tree.get(), source->GetName());
            } else {
                leaf = std::make_unique<iGame::iGameEncodeAdapter>(source);
                request.input = datacodec::EncodeInput::LeafAdapter(leaf.get());
            }
            request.output = datacodec::EncodeOutput::ByteRange(sink, kind);
            request.runRecordSink = records;
            request.resources = resources;
            if (memoryTrace) {
                datacodec::DataCodecExecutionResources root(resources);
                datacodec::CodecRunScope scope(root);
                if (!scope) { throw std::runtime_error("investigation encode could not begin"); }
                datacodec::test::MemoryInvestigationTrace trace(root, "encode");
                encoded = datacodec::EncodeInRun(request, root);
                if (encoded.failure) { root.RecordFailure(*encoded.failure); }
                encoded.success = scope.Finish(encoded.success);
                trace.Finish();
            } else { encoded = datacodec::Encode(request); }
        }
        const auto encodeSeconds = Seconds(start);
        const auto encodeCpuEnd = CpuSeconds();
        std::cout << "RESULT encode_success=" << encoded.success << " encode_seconds=" << encodeSeconds
                  << " encoded_bytes=" << encoded.encodedByteCount;
        if (encodeCpuStart >= 0.0 && encodeCpuEnd >= encodeCpuStart) {
            std::cout << " encode_cpu_seconds=" << encodeCpuEnd - encodeCpuStart;
        }
        std::cout << std::endl;
        Memory("after_encode");
        if (!encoded.success) {
            for (const auto& m : encoded.messages) { std::cerr << m.code << ' ' << m.text << ' ' << m.technicalDetail << '\n'; }
            return 4;
        }
        // 独立解码前释放宿主源对象，避免保留两份完整模型
        source = nullptr;
        reader = nullptr;
        Memory("before_decode");
        start = Clock::now();
        std::cout << "BEGIN decode" << std::endl;
        iGame::DataCodecDataObjectDecodeRequest request;
        request.inputReader = std::make_shared<::datacodec::FileByteRangeReader>(output);
        request.loadAllAvailableAttributes = true;
        request.runRecordSink = records;
        request.resources = resources;
        iGame::DataCodecDataObjectDecodeResult decoded;
        if (memoryTrace) {
            decoded = DecodeWithMemoryTrace(request.inputReader, resources, records);
        } else { decoded = iGame::DecodeDataCodecDataObject(request); }
        const auto decodeSeconds = Seconds(start);
        std::cout << "RESULT decode_success=" << decoded.success << " decode_seconds=" << decodeSeconds
                  << " decoded_cache_hit=" << decoded.decodedFrameCacheHit << std::endl;
        Memory("after_decode");
        if (!decoded.success || decoded.output == nullptr) {
            for (const auto& m : decoded.messages) { std::cerr << m.code << ' ' << m.text << ' ' << m.technicalDetail << '\n'; }
            return 5;
        }
        const auto decodedShape = Describe(decoded.output, "decoded");
        const bool shapeMatches = sourceShape == decodedShape;
        std::cout << "RESULT shape_matches=" << shapeMatches << " source_file_bytes=" << std::filesystem::file_size(input)
                  << " encoded_file_bytes=" << std::filesystem::file_size(output)
                  << " load_seconds=" << loadSeconds << " encode_seconds=" << encodeSeconds
                  << " decode_seconds=" << decodeSeconds << std::endl;
        return shapeMatches ? 0 : 6;
    } catch (const std::exception& error) {
        std::cerr << "BENCHMARK_FAILURE " << error.what() << std::endl;
        return 7;
    }
}
