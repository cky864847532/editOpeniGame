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
#include <charconv>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <thread>
#include <cmath>

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

}

int main(int argc, char** argv) {
    try {
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
        for (int i = 3; i < argc; i += 2) {
            const std::string_view option(argv[i]), value(argv[i + 1]);
            if (option == "--memory") {
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
            encoded = datacodec::Encode(request);
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
        const auto decoded = iGame::DecodeDataCodecDataObject(request);
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
