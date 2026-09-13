#include "DataCodec/Filter/Test/Feature/iGameDataCodecFeaturePlaybackSession.h"
#include "DataCodec/Filter/Test/Feature/iGameDataCodecFeaturePreparedSurfaceAttributes.h"
#include "DataCodec/Filter/Test/Feature/iGameDataCodecFeatureRemap.h"
#include "DataCodec/Filter/Test/Feature/iGameDataCodecFeatureLocalization.h"
#include "DataCodec/Filter/Test/Feature/iGameDataCodecFeatureNativeDecodeStorage.h"
#include "DataCodec/Filter/Adapter/iGameDataCodecAttributeCatalog.h"
#include "DataCodec/Filter/Output/iGameDataCodecOutputSinks.h"
#include "DataCodec/Log/Report/DataCodecProcessReportJson.h"
#include "DataCodec/Test/Suite/DataCodecTestSuite.h"
#include "DataCodec/Test/Experiment/DataCodecResourcePerformance.h"
#include "DataCodec/Test/Experiment/DataCodecResourceEnvironment.h"
#include "DataCodec/Test/Feature/DataCodecFeatureEncodedInputCache.h"
#include "DataCodec/Test/Feature/DataCodecFeatureDecodedFrameCache.h"
#include "DataCodec/Test/Feature/DataCodecFeatureDecodeReferenceCache.h"
#include "DataCodec/Filter/Telemetry/iGameDataCodecTelemetryCapture.h"
#include "IGDC/iGameIGDCReader.h"
#include "IGDC/iGameIGDCWriter.h"
#include "iGameFilterIncludes.h"

#include <chrono>
#include <charconv>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct ObservedTestCheck {
    std::array<char, 192u> name{};
    bool passed{};
};

// 固定存储采集实际执行的断言，不在分配失败注入期间分配或输出
std::array<ObservedTestCheck, 16384u> observedTestChecks;
std::atomic_size_t observedTestCheckCount{0u};
std::atomic_size_t omittedTestCheckCount{0u};

void ObserveTestCheck(bool passed, std::string_view name) noexcept {
    const auto index = observedTestCheckCount.fetch_add(1u, std::memory_order_relaxed);
    if (index >= observedTestChecks.size() || name.size() >= observedTestChecks[0].name.size()) {
        omittedTestCheckCount.fetch_add(1u, std::memory_order_relaxed);
        return;
    }
    auto& item = observedTestChecks[index];
    std::memcpy(item.name.data(), name.data(), name.size());
    item.name[name.size()] = '\0';
    item.passed = passed;
}

bool PrintObservedTestChecks() {
    std::map<std::string, std::pair<std::size_t, std::size_t>> counts;
    const auto end = std::min(observedTestChecks.size(), observedTestCheckCount.load());
    for (std::size_t i = 0u; i < end; ++i) {
        const auto& item = observedTestChecks[i];
        if (item.name[0] == '\0') { continue; }
        auto& count = counts[item.name.data()];
        if (item.passed) { ++count.first; } else { ++count.second; }
    }
    for (const auto& [name, count] : counts) {
        std::cout << "executed_check," << name << ',' << count.first << ',' << count.second << '\n';
    }
    std::cout << "executed_check_summary,total=" << observedTestCheckCount.load()
        << ",unique=" << counts.size() << ",omitted=" << omittedTestCheckCount.load() << '\n';
    return omittedTestCheckCount.load() == 0u;
}

void PrintResult(const datacodec::test::TestResult& result) {
    for (const auto& diagnostic : result.diagnostics) {
        std::cout << diagnostic << '\n';
    }
    for (const auto& failure : result.failures) {
        std::cerr << failure.check << ": " << failure.message << '\n';
    }
}

bool IsDataCodecReportFileTimestamp(const std::string_view value) {
    constexpr std::size_t timestampLength = 20u;
    if (value.size() != timestampLength ||
        value[8] != '_' || value[15] != '_' || value[19] != 'Z') {
        return false;
    }
    for (std::size_t index = 0u; index < value.size(); ++index) {
        if (index == 8u || index == 15u || index == 19u) {
            continue;
        }
        if (value[index] < '0' || value[index] > '9') {
            return false;
        }
    }
    return true;
}

std::optional<std::string> DataCodecReportTimestampFromPath(
    const std::filesystem::path& path,
    const std::string_view prefix) {
    const auto filename = path.filename().string();
    constexpr std::string_view extension = ".json";
    if (!filename.starts_with(prefix) || !filename.ends_with(extension)) {
        return std::nullopt;
    }
    const auto timestampLength = filename.size() - prefix.size() - extension.size();
    const auto timestamp = filename.substr(prefix.size(), timestampLength);
    return IsDataCodecReportFileTimestamp(timestamp)
        ? std::optional<std::string>{timestamp}
        : std::nullopt;
}

bool CollectTimestampedDataCodecReports(
    const std::filesystem::path& directory,
    const std::string_view prefix,
    std::vector<std::filesystem::path>& paths) {
    paths.clear();
    if (!std::filesystem::is_directory(directory)) {
        return false;
    }
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto filename = entry.path().filename().string();
        if (!filename.starts_with(prefix)) {
            continue;
        }
        if (!DataCodecReportTimestampFromPath(entry.path(), prefix).has_value()) {
            return false;
        }
        paths.push_back(entry.path());
    }
    return true;
}

int TestDataCodecReportFileContract() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path() /
        ("igame_datacodec_report_contract_" + std::to_string(unique));
    std::error_code errorCode;
    std::filesystem::create_directories(directory, errorCode);
    if (errorCode) {
        std::cerr << "failed to create report contract directory\n";
        return 1;
    }
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    } cleanup{directory};

    auto sink = std::make_shared<iGame::iGameDataCodecReportFileSink>(
        directory,
        iGame::iGameDataCodecReportFileMode::Replace);
    const auto reportFileTimestamp = datacodec::MakeDataCodecReportFileTimestampUtc();
    if (!IsDataCodecReportFileTimestamp(reportFileTimestamp)) {
        std::cerr << "report filename timestamp contract failed\n";
        return 1;
    }
    const auto processStem = "encode_process_" + reportFileTimestamp;
    const datacodec::DataCodecProcessReport runningReport{
        .operation = datacodec::TelemetryRunKind::Encode,
        .generatedAtUtc = "2026-01-01T00:00:00Z",
        .objectName = "fixture.igc",
        .completed = false,
        .processes = {
            datacodec::DataCodecProcessNode{
                .name = "EncodeData",
                .completed = false,
            },
        },
    };
    const auto runningResult = sink->WriteReportFile({
        .name = processStem,
        .mediaType = "application/json",
        .preferredExtension = ".json",
        .content = datacodec::SerializeDataCodecProcessReportJson(runningReport),
    });
    auto completedReport = runningReport;
    completedReport.completed = true;
    completedReport.success = true;
    completedReport.processes.front().completed = true;
    const auto completedResult = sink->WriteReportFile({
        .name = processStem,
        .mediaType = "application/json",
        .preferredExtension = ".json",
        .content = datacodec::SerializeDataCodecProcessReportJson(completedReport),
    });
    const auto processPath = directory / (processStem + ".json");
    std::ifstream processInput(processPath, std::ios::binary);
    const std::string processText{
        std::istreambuf_iterator<char>(processInput),
        std::istreambuf_iterator<char>()};
    std::size_t fileCount = 0u;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file()) {
            ++fileCount;
        }
    }
    if (!runningResult.success || !completedResult.success || fileCount != 1u ||
        processText.find("\"status\": \"success\"") == std::string::npos) {
        std::cerr << "report replacement contract failed\n";
        return 1;
    }

    const datacodec::DataCodecErrorReport errorReport{
        .operation = datacodec::TelemetryRunKind::Encode,
        .generatedAtUtc = "2026-01-01T00:00:00Z",
        .objectName = "fixture.igc",
        .errors = {
            datacodec::DataCodecErrorReportEntry{
                .phasePath = {"Encode", "Verification"},
                .message = "verification failed",
            },
        },
    };
    const auto errorStem = "encode_errors_" + reportFileTimestamp;
    const auto errorResult = sink->WriteReportFile({
        .name = errorStem,
        .mediaType = "application/json",
        .preferredExtension = ".json",
        .content = datacodec::SerializeDataCodecErrorReportJson(errorReport),
    });
    const auto errorPath = directory / (errorStem + ".json");
    const auto processTimestamp = DataCodecReportTimestampFromPath(
        processPath,
        "encode_process_");
    const auto errorTimestamp = DataCodecReportTimestampFromPath(
        errorPath,
        "encode_errors_");
    if (!errorResult.success || !std::filesystem::is_regular_file(errorPath) ||
        !processTimestamp.has_value() || !errorTimestamp.has_value() ||
        *processTimestamp != *errorTimestamp) {
        std::cerr << "paired report timestamp contract failed\n";
        return 1;
    }

    const auto source = iGame::datacodec_test::BuildSyntheticSurfaceSmokeObject();
    const auto validPath = directory / "decode_report_fixture.igc";
    auto writer = iGame::IGDCWriter::New();
    if (source == nullptr || writer == nullptr ||
        !writer->WriteToFile(source, validPath.string())) {
        std::cerr << "failed to prepare decode report fixture\n";
        return 1;
    }
    auto decodeOptions = datacodec::DataCodecDecodeOptions{};
    decodeOptions.logging.enableFileLog = true;
    decodeOptions.logging.enableConsoleLog = false;
    auto reader = iGame::IGDCReader::New();
    reader->SetDecodeOptions(decodeOptions);
    reader->SetFilePath(validPath.string());
    if (!reader->Execute()) {
        std::cerr << "decode report fixture failed to decode\n";
        return 1;
    }
    const auto decodeReportDirectory = directory / "decode_report_fixture_Decode_Report";
    std::vector<std::filesystem::path> decodeProcessPaths;
    std::vector<std::filesystem::path> decodeErrorPaths;
    if (!CollectTimestampedDataCodecReports(
            decodeReportDirectory,
            "decode_process_",
            decodeProcessPaths) ||
        !CollectTimestampedDataCodecReports(
            decodeReportDirectory,
            "decode_errors_",
            decodeErrorPaths) ||
        decodeProcessPaths.size() != 1u || !decodeErrorPaths.empty() ||
        std::filesystem::exists(decodeReportDirectory / "decode_process.json") ||
        std::filesystem::exists(decodeReportDirectory / "decode_errors.json")) {
        std::cerr << "successful decode report file contract failed\n";
        return 1;
    }
    for (const auto& entry : std::filesystem::directory_iterator(decodeReportDirectory)) {
        const auto extension = entry.path().extension().string();
        if (extension == ".csv" || entry.path().filename().string().starts_with("00_")) {
            std::cerr << "decode report emitted a legacy detail file\n";
            return 1;
        }
    }

    const auto invalidPath = directory / "invalid_decode_report.igc";
    {
        std::ofstream invalidOutput(invalidPath, std::ios::binary | std::ios::trunc);
        invalidOutput << "invalid";
    }
    auto invalidReader = iGame::IGDCReader::New();
    invalidReader->SetDecodeOptions(decodeOptions);
    invalidReader->SetFilePath(invalidPath.string());
    if (invalidReader->Execute()) {
        std::cerr << "invalid decode report fixture unexpectedly succeeded\n";
        return 1;
    }
    const auto invalidReportDirectory = directory / "invalid_decode_report_Decode_Report";
    std::vector<std::filesystem::path> invalidProcessPaths;
    std::vector<std::filesystem::path> invalidErrorPaths;
    const auto invalidFilesValid = CollectTimestampedDataCodecReports(
            invalidReportDirectory,
            "decode_process_",
            invalidProcessPaths) &&
        CollectTimestampedDataCodecReports(
            invalidReportDirectory,
            "decode_errors_",
            invalidErrorPaths);
    if (!invalidFilesValid || invalidProcessPaths.size() != 1u ||
        invalidErrorPaths.size() != 1u) {
        std::cerr << "failed decode report file contract failed\n";
        return 1;
    }
    const auto invalidProcessTimestamp = DataCodecReportTimestampFromPath(
        invalidProcessPaths.front(),
        "decode_process_");
    const auto invalidErrorTimestamp = DataCodecReportTimestampFromPath(
        invalidErrorPaths.front(),
        "decode_errors_");
    if (!invalidProcessTimestamp.has_value() ||
        !invalidErrorTimestamp.has_value() ||
        *invalidProcessTimestamp != *invalidErrorTimestamp) {
        std::cerr << "failed decode report timestamps do not match\n";
        return 1;
    }
    std::cout << "DataCodec report file contract tests passed\n";
    return 0;
}

int WriteBrowserFixture(const std::filesystem::path& outputPath) {
    const auto source = iGame::datacodec_test::BuildSyntheticSurfaceSmokeObject();
    auto writer = iGame::IGDCWriter::New();
    std::vector<iGame::DataCodecEncodeAttributeDescriptor> descriptors;
    if (source == nullptr || writer == nullptr ||
        !iGame::CollectDataCodecEncodeRepresentativeAttributeCatalog(source, descriptors)) {
        std::cerr << "failed to collect browser fixture attributes\n";
        return 1;
    }

    std::vector<datacodec::AttributeTarget> targets;
    targets.reserve(descriptors.size());
    for (const auto& descriptor : descriptors) { targets.push_back(descriptor.target); }
    if (targets.empty()) {
        std::cerr << "browser fixture has no attributes\n";
        return 1;
    }

    writer->SetAttributeTargets(std::move(targets));
    writer->SetFilePath(outputPath.string());
    iGame::iGameDataCodecTelemetryCapture recordSinks;
    recordSinks.CaptureSessions(
        ::datacodec::kRunLifecycleRecordMask |
        ::datacodec::RunRecordKind::Message);
    writer->SetTelemetrySink(recordSinks.Sink());
    if (!writer->WriteToFile(source, outputPath.string())) {
        for (const auto& message : recordSinks.SnapshotMessages()) {
            std::cerr << message.text << '\n';
        }
        std::cerr << "failed to write browser fixture\n";
        return 1;
    }
    for (const auto& path : writer->GetWrittenFilePaths()) {
        std::cout << path << '\n';
    }
    return 0;
}

}

namespace datacodec::test { int RunDataCodecStorageAnalysisTests(); }
namespace datacodec::test { int RunDataCodecEncodeStorageAnalysisTests(); }

int main(const int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--encode-storage-analysis") {
        return datacodec::test::RunDataCodecEncodeStorageAnalysisTests();
    }
    if (argc == 2 && std::string_view(argv[1]) == "--storage-analysis") {
        return datacodec::test::RunDataCodecStorageAnalysisTests();
    }
    if (argc == 2 && std::string_view(argv[1]) == "--resource-coverage") {
        datacodec::test::testCheckObserver = ObserveTestCheck;
        const auto result = datacodec::test::RunDataCodecSelfTest();
        const bool inputCache = datacodec::test::RunDataCodecFeatureEncodedInputCache() == 0;
        const bool frameCache = datacodec::test::RunDataCodecFeatureDecodedFrameCache() == 0;
        const bool referenceCache = datacodec::test::RunDataCodecFeatureDecodeReferenceCache() == 0;
        datacodec::test::testCheckObserver = nullptr;
        PrintResult(result);
        const bool completeTrace = PrintObservedTestChecks();
        return result.passed && inputCache && frameCache && referenceCache && completeTrace ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--resource-environment") {
        try {
            const auto result = datacodec::test::RunDataCodecResourceEnvironment();
            PrintResult(result);
            return result.passed ? 0 : 1;
        } catch (const std::exception& failure) {
            std::cerr << "resource environment experiment failed: " << failure.what() << '\n';
            return 1;
        }
    }
    if (argc == 2 && std::string_view(argv[1]) == "--remap-contract") {
        return iGame::datacodec_test::RunDataCodecFeatureRemap();
    }
    if (argc == 2 && std::string_view(argv[1]) == "--resource-overhead") {
        const auto result = datacodec::test::RunDataCodecResourceOverhead();
        PrintResult(result);
        return result.passed ? 0 : 1;
    }
    if (argc >= 2 && std::string_view(argv[1]) == "--resource-performance") {
        if (argc != 7 && argc != 8) {
            std::cerr << "usage: --resource-performance tuples repetitions storage_MiB max_threads audit_0_or_1 [points|many-fields|correlated-fields|variable-topology|morton]\n";
            return 2;
        }
        std::array<std::uint64_t, 5u> values{};
        for (std::size_t i = 0u; i < values.size(); ++i) {
            const std::string_view text(argv[i + 2u]);
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), values[i]);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) { return 2; }
        }
        constexpr std::uint64_t MiB = 1024u * 1024u;
        if (values[0] > std::numeric_limits<std::size_t>::max() || values[1] > 100u ||
            values[2] > std::numeric_limits<std::uint64_t>::max() / MiB ||
            values[3] > std::numeric_limits<std::size_t>::max() || values[4] > 1u) { return 2; }
        try {
            const auto result = datacodec::test::RunDataCodecResourcePerformance(
                values[0], values[1], values[2] * MiB, values[3], values[4] != 0u,
                argc == 8 ? std::string_view(argv[7]) : std::string_view("points"));
            PrintResult(result);
            return result.passed ? 0 : 1;
        } catch (const std::exception& failure) {
            std::cerr << "resource experiment failed: " << failure.what() << '\n';
            return 1;
        }
    }
    if (argc == 2 && std::string_view(argv[1]) == "--resource-mode-contract") {
        const auto result = datacodec::test::RunDataCodecResourcePerformance(17u, 1u, 16u * 1024u * 1024u, 2u);
        PrintResult(result);
        return result.passed ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--numeric-decode-execution") {
        const auto result = datacodec::test::RunDataCodecFeatureNumericDecodeExecution();
        PrintResult(result);
        return result.passed ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--numeric-execution") {
        const auto result = datacodec::test::RunDataCodecFeatureNumericExecution();
        PrintResult(result);
        return result.passed ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--topology-execution") {
        const auto result = datacodec::test::RunDataCodecFeatureTopologyExecution();
        PrintResult(result);
        return result.passed ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--prepared-surface") {
        return iGame::datacodec_test::RunDataCodecFeaturePreparedSurfaceAttributes();
    }
    if (argc == 2 && std::string_view(argv[1]) == "--morton-resources") {
        const auto result = datacodec::test::RunDataCodecFeatureMortonResources();
        PrintResult(result);
        return result.passed ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--execution-mechanism") {
        const auto result = datacodec::test::RunDataCodecFeatureExecutionMechanism();
        PrintResult(result);
        return result.passed ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--cpu-control") {
        const auto result = datacodec::test::RunDataCodecCpuControlTests();
        PrintResult(result);
        return result.passed ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--task-coordinator") {
        return datacodec::test::RunDataCodecFeatureDecodeTaskCoordinator();
    }
    if (argc == 2 && std::string_view(argv[1]) == "--native-decode-storage") {
        const auto result = datacodec::test::RunDataCodecFeatureNativeDecodeStorage();
        PrintResult(result);
        return result.passed ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--storage-ownership") {
        const auto result = datacodec::test::RunDataCodecFeatureStorageOwnership();
        PrintResult(result);
        return result.passed ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--failure-contract") {
        const auto result = datacodec::test::RunDataCodecFeatureFailure();
        PrintResult(result);
        return result.passed ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--localization") {
        const auto result = datacodec::test::RunDataCodecFeatureLocalization();
        PrintResult(result);
        const auto hostResult = iGame::datacodec_test::RunDataCodecFeatureHostLocalization();
        return result.passed && hostResult == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--report-contract") {
        const auto result = datacodec::test::RunDataCodecSelfTest();
        PrintResult(result);
        if (!result.passed) {
            std::cerr << "DataCodec core tests failed\n";
            return 1;
        }
        return TestDataCodecReportFileContract();
    }
    if (argc == 3 && std::string(argv[1]) == "--write-browser-fixture") {
        return WriteBrowserFixture(argv[2]);
    }

    const auto coreResult = datacodec::test::RunDataCodecSelfTest();
    PrintResult(coreResult);
    if (!coreResult.passed) {
        std::cerr << "DataCodec core tests failed\n";
        return 1;
    }

    if (TestDataCodecReportFileContract() != 0) {
        return 1;
    }

    if (datacodec::test::RunDataCodecFeatureEncodedInputCache() != 0) {
        return 1;
    }

    if (iGame::datacodec_test::RunDataCodecFeatureRemap() != 0) {
        std::cerr << "iGame DataCodec remap tests failed\n";
        return 1;
    }

    if (iGame::datacodec_test::RunDataCodecFeaturePlaybackSession(argc, argv) != 0) {
        std::cerr << "iGame DataCodec integration tests failed\n";
        return 1;
    }

    if (iGame::datacodec_test::RunDataCodecFeaturePreparedSurfaceAttributes() != 0) {
        std::cerr << "iGame prepared surface attribute tests failed\n";
        return 1;
    }

    std::cout << "DataCodec tests passed\n";
    return 0;
}
