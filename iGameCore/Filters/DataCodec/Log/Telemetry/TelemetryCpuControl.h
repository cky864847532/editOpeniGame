#pragma once

#include "DataCodec/Runtime/Execution/DataCodecExecutionResources.h"
#include "DataCodec/API/Adapter/RunRecord.h"
#include <string>

namespace datacodec {

// 请求内累计快照复用已有消息通道，诊断失败不影响编解码
template<class Context>
void RecordCpuControlSummary(Context& context) noexcept {
    if (!context.runRecords.Wants(RunRecordKind::Message)) { return; }
    context.runRecords.TryExport([&] {
        ResourceDebugSnapshot state;
        if (!context.resources.TryCopyResourceDebugSnapshot(state)) {
            context.resources.RecordDiagnosticExportFailure();
            return;
        }
        const auto& cpu = state.cpu;
        std::string detail = "request=" + std::to_string(state.requestId) +
            ";mode=" + CodecThreadModeName(cpu.mode) +
            ";effectiveComputeLimit=" + std::to_string(state.effectiveComputeLimit);
        if (cpu.mode == CodecThreadMode::Adaptive) {
            detail += ";targetIdle=" + std::to_string(cpu.targetIdleRatio) +
                ";observedIdle=" + (cpu.observedIdleRatio ? std::to_string(*cpu.observedIdleRatio) : "unknown") +
                ";quota=" + std::to_string(cpu.quota) +
                ";gainSource=" + (cpu.response ? "observed" : "nominal") +
                ";response=" + (cpu.response ? std::to_string(*cpu.response) : "unknown") +
                ";shrinkGain=" + std::to_string(cpu.shrinkGain) +
                ";growthGain=" + std::to_string(cpu.growthGain) +
                ";calibration=" + CpuCalibrationResultName(cpu.calibration) +
                ";calibrationMs=" + std::to_string(std::chrono::duration<double, std::milli>(cpu.calibrationElapsed).count()) +
                ";gainUpdates=" + std::to_string(cpu.gainUpdates) +
                ";adjustments=" + std::to_string(cpu.adjustments) +
                ";throttleMs=" + std::to_string(std::chrono::duration<double, std::milli>(state.cpuThrottleDuration).count()) +
                ";permitCreditSeconds=" + std::to_string(state.cpuPermitCredit) +
                ";lastDecision=" + CpuDecisionReasonName(state.cpuDecision);
        }
        context.AddInfo("CpuControl", std::move(detail));
    });
}

}
