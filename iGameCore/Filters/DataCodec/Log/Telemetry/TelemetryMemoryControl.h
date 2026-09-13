#pragma once

#include "DataCodec/Runtime/Execution/DataCodecResourceController.h"
#include "DataCodec/API/Adapter/RunRecord.h"
#include <string>
#include <cstdlib>

namespace datacodec {

// 临时调度调查插桩，仅由显式环境开关启用，沿用现有消息通道
template<class Context, class Workspace>
void RecordSchedulerInvestigation(Context& context, Workspace& workspace, const std::string& point) noexcept {
    if (std::getenv("IGAME_DATACODEC_SCHEDULER_TRACE") == nullptr ||
        !context.runRecords.Wants(RunRecordKind::Message)) { return; }
    context.runRecords.TryExport([&] {
        const auto capacity = context.resources.StorageCapacity()->Snapshot();
        const auto resident = [](const auto& store) { return store ? store->ResidentSizeHint() : 0u; };
        const auto stores = workspace.ByteStoreSessionRef().SnapshotStats();
        const auto& topo = workspace.topology;
        context.AddInfo("SchedulerInvestigation", "point=" + point +
            ";atMs=" + std::to_string(std::chrono::duration<double, std::milli>(ResourceClock::now().time_since_epoch()).count()) +
            ";reserved=" + std::to_string(capacity.reservedBytes) +
            ";geometry=" + std::to_string(resident(workspace.geometry.bytes)) +
            ";connectivity=" + std::to_string(topo ? resident(topo->connectivity) : 0u) +
            ";offsets=" + std::to_string(topo ? resident(topo->offsets) : 0u) +
            ";cellTypes=" + std::to_string(topo ? resident(topo->cellTypes) : 0u) +
            ";topologySharedOwners=" + std::to_string(topo.use_count()) +
            ";nativeHint=" + std::to_string(context.adapter ? context.adapter->NativeResidentBytesHint() : 0u) +
            ";storeMemory=" + std::to_string(stores.residentBytes) + ";storeFile=" + std::to_string(stores.managedFileBytes));
    });
}

// 许可、预约和系统观测分别输出，未知字段保留 null，复用现有消息通道
template<class Context>
void RecordMemoryControlSummary(Context& context) noexcept {
    if (!context.runRecords.Wants(RunRecordKind::Message)) { return; }
    context.runRecords.TryExport([&] {
        ResourceDebugSnapshot s;
        if (!context.resources.TryCopyResourceDebugSnapshot(s)) {
            context.resources.RecordDiagnosticExportFailure();
            return;
        }
        const auto text = [](const auto& value) { return value ? std::to_string(*value) : std::string("null"); };
        const auto ms = [](auto duration) { return std::to_string(std::chrono::duration<double, std::milli>(duration).count()); };
        const auto at = [&](const auto& value) { return value ? ms(value->time_since_epoch()) : std::string("null"); };
        const auto& m = s.memory;
        std::optional<ResourceClock::time_point> lastTrim;
        for (std::size_t i = 0u; i < s.eventCount; ++i) {
            if (s.events[i].kind == ResourceEventKind::TrimCompleted && (!lastTrim || s.events[i].at > *lastTrim)) {
                lastTrim = s.events[i].at;
            }
        }
        std::string detail = "request=" + std::to_string(s.requestId) + ";mode=" + CodecResourceModeName(s.mode) +
            ";reservedBytes=" + std::to_string(s.storage.reservedBytes) +
            ";currentLimitBytes=" + text(s.limits.ownedStorageLimitBytes);
        if (s.mode == CodecResourceMode::Adaptive) {
            const char* phase = "Normal";
            switch (s.controlPhase) {
            case ResourceControlPhase::Drain: phase = "Drain"; break;
            case ResourceControlPhase::Hold: phase = "Hold"; break;
            case ResourceControlPhase::Recover: phase = "Recover"; break;
            default: break;
            }
            detail += ";targetAvailableMemoryRatio=" + std::to_string(m.targetRatio) +
                ";physicalTotalBytes=" + text(m.physicalTotalBytes) + ";availablePhysicalBytes=" + text(m.availableBytes) +
                ";reserveBytes=" + std::to_string(m.reserveBytes) + ";recoveryBytes=" + std::to_string(m.recoveryBytes) +
                ";absoluteCapacityBoundBytes=" + std::to_string(m.absoluteCapacityBytes) +
                ";environmentRemainingBytes=" + text(m.environmentRemainingBytes) +
                ";commitRemainingBytes=" + text(s.resourceSample.commitRemainingBytes) +
                ";jobRemainingBytes=" + text(s.resourceSample.jobRemainingBytes) +
                ";processRemainingBytes=" + text(s.resourceSample.processRemainingBytes) +
                ";addressSpaceRemainingBytes=" + text(s.resourceSample.addressSpaceRemainingBytes) +
                ";growthHeadroomBytes=" + std::to_string(m.growthHeadroomBytes) +
                ";unreservedAllowanceBytes=" + std::to_string(s.limits.ownedStorageLimitBytes.value_or(0u) -
                    std::min(s.limits.ownedStorageLimitBytes.value_or(0u), s.storage.reservedBytes)) +
                ";nextRequiredBytes=" + std::to_string(s.nextWorkBytes) +
                ";demand=" + (s.memoryDemandKind == MemoryDemandKind::RequiredContinuation ? "RequiredContinuation" : "Block") +
                ";requester=" + s.memoryRequester.label.data() + ";demandId=" + std::to_string(s.memoryDemandId) +
                ";workPath=" + std::to_string(static_cast<unsigned>(s.workType.path)) +
                ";grantEpoch=" + std::to_string(m.grantEpoch) + ";gateOpen=" + (s.gateOpen ? "true" : "false") +
                ";phase=" + phase + ";sampleAgeMs=" + ms(s.capturedAt - s.resourceSample.sampledAt) +
                ";sampleValid=" + (ValidPhysicalMemorySample(s.resourceSample, s.capturedAt) ? "true" : "false") +
                ";waitStartedMs=" + at(m.waitSince) +
                ";waitDeadlineMs=" + (m.waitSince ? ms((*m.waitSince + resource_control::holdTimeout).time_since_epoch()) : "null") +
                ";lastPreparationMs=" + at(m.lastPreparationAt) + ";trimEpoch=" + std::to_string(s.trimEpoch) +
                ";lastTrimCompletedMs=" + at(lastTrim) +
                ";previewRatio=" + std::to_string(m.previewRatio) +
                ";previewHeadroomBytes=" + std::to_string(m.previewHeadroomBytes) +
                ";lastDecision=" + MemoryDecisionReasonName(m.reason);
        }
        context.AddInfo("MemoryControl", std::move(detail));
    });
}

}
