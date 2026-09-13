#include "DataCodec/Filter/Adapter/iGamePreparedSurfaceDecodeAdapter.h"

#include "DataCodec/Validation/Common/DataCodecValidation.h"
#include "ModelSurface/iGameModelGeometryFilter.h"
#include "iGameDrawObject.h"
#include "iGameSurfaceMesh.h"
#include "iGameUnstructuredMesh.h"

#include <chrono>
#include <atomic>
#include <functional>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

IGAME_NAMESPACE_BEGIN

namespace {

void CollectUnstructuredMeshes(
    const DataObject::Pointer& root,
    std::vector<UnstructuredMesh::Pointer>& meshes) {
    if (root == nullptr) {
        return;
    }
    auto mesh = DynamicCast<UnstructuredMesh>(root);
    if (mesh != nullptr) {
        meshes.push_back(std::move(mesh));
    }
    if (!root->HasSubDataObject()) {
        return;
    }
    for (auto iterator = root->SubDataObjectIteratorBegin();
         iterator != root->SubDataObjectIteratorEnd();
         ++iterator) {
        CollectUnstructuredMeshes(iterator->second, meshes);
    }
}

} // 匿名命名空间

struct iGamePreparedSurfaceDecodeAdapter::Impl {
    struct BlockProgress {
        std::size_t cellOffset{0u};
        std::size_t cellCount{0u};
        // 0 待处理，1 正在处理，2 已完成，3 失败
        unsigned char state{0u};
    };

    bool BeginSurface(
        const ::datacodec::ConnectivityTopologyDecodeInfo& info,
        std::string* error) {
        std::shared_ptr<ModelGeometryDecodedSurfaceBuilder> retired;
        std::lock_guard<std::mutex> lock(mutex);
        if (activeBlocks != 0u) {
            return ::datacodec::validation::AssignError(error, "prepared surface workers have not drained");
        }
        retired = std::move(builder);
        completed = false;
        failed = false;
        expectedBlockCount = info.blockCount;
        completedBlockCount = 0u;
        expectedCellCount = info.cellCount;
        completedCellCount = 0u;
        peakActiveBlocks = 0u;
        workerCapacity = info.workerCapacity;
        blockProgress.assign(info.blockCount, BlockProgress{});
        diagnosticsIncomplete.store(false, std::memory_order_relaxed);
        summary.clear();
        accumulateCpuMs = 0.0;
        startedAt = std::chrono::steady_clock::now();
        fixedCellSize = info.fixedCellSize;
        hasOffsets = info.hasOffsets;
        hasCellTypes = info.hasCellTypes;
        if (!info.hasCellTypes || info.pointCount == 0u || info.cellCount == 0u ||
            info.blockCount == 0u || workerCapacity == 0u) {
            if (error != nullptr) {
                *error = "prepared surface requires explicit cell types and non-empty topology";
            }
            return false;
        }
        builder = std::make_shared<ModelGeometryDecodedSurfaceBuilder>(
            static_cast<IGsize>(info.pointCount),
            workerCapacity);
        return true;
    }

    bool AccumulateSurfaceBlock(
        ::datacodec::DecodedConnectivityTopologyBlock block,
        std::string* error) {
        // 锁只保护生命周期和统计，表面提取在原有 worker 中并发执行
        std::shared_ptr<ModelGeometryDecodedSurfaceBuilder> localBuilder;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (builder == nullptr || completed || failed || block.blockIndex >= expectedBlockCount ||
                blockProgress[block.blockIndex].state != 0u || block.workerIndex >= workerCapacity ||
                block.fixedCellSize != fixedCellSize || block.cellOffset > expectedCellCount ||
                block.cellTypes.empty() || block.cellTypes.size() > expectedCellCount - block.cellOffset) {
                failed = true;
                return ::datacodec::validation::AssignError(error, "prepared surface is not accepting topology blocks");
            }
            blockProgress[block.blockIndex] = {block.cellOffset, block.cellTypes.size(), 1u};
            ++activeBlocks;
            peakActiveBlocks = std::max(peakActiveBlocks, activeBlocks);
            localBuilder = builder;
        }
        const auto blockStartedAt = std::chrono::steady_clock::now();
        bool success = false;
        try {
        success = localBuilder->AccumulateBlock(
            block.workerIndex,
            block.cellOffset,
            block.fixedCellSize,
            block.connectivity,
            block.offsets,
            block.cellTypes,
            error);
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex);
            failed = true;
            blockProgress[block.blockIndex].state = 3u;
            --activeBlocks;
            throw;
        }
        const auto elapsedMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - blockStartedAt).count();
        std::lock_guard<std::mutex> lock(mutex);
        accumulateCpuMs += elapsedMs;
        failed |= !success;
        --activeBlocks;
        blockProgress[block.blockIndex].state = success ? 2u : 3u;
        if (success) {
            ++completedBlockCount;
            completedCellCount += block.cellTypes.size();
        }
        return success;
    }

    bool EndSurface(std::string* error) {
        std::shared_ptr<ModelGeometryDecodedSurfaceBuilder> retired;
        std::lock_guard<std::mutex> lock(mutex);
        if (!builder || failed || completed || activeBlocks != 0u || completedBlockCount != expectedBlockCount ||
            completedCellCount != expectedCellCount) {
            failed = true;
            retired = std::move(builder);
            return ::datacodec::validation::AssignError(error, "prepared surface topology is incomplete");
        }
        std::size_t nextCell = 0u;
        for (const auto& progress : blockProgress) {
            if (progress.state != 2u || progress.cellOffset != nextCell ||
                progress.cellCount > expectedCellCount - nextCell) {
                failed = true;
                retired = std::move(builder);
                return ::datacodec::validation::AssignError(error, "prepared surface cell ranges are incomplete");
            }
            nextCell += progress.cellCount;
        }
        completed = true;
        finishedAt = std::chrono::steady_clock::now();
        return true;
    }

    bool AttachPreparedSurface(
        const DataObject::Pointer& root,
        std::string* error) {
        std::shared_ptr<ModelGeometryDecodedSurfaceBuilder> localBuilder;
        double localAccumulateCpuMs = 0.0;
        std::chrono::steady_clock::time_point localStartedAt;
        std::chrono::steady_clock::time_point localFinishedAt;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!completed || failed || builder == nullptr) {
                if (error != nullptr) {
                    *error = "prepared surface construction did not complete";
                }
                return false;
            }
            localBuilder = std::move(builder);
            localAccumulateCpuMs = accumulateCpuMs;
            localStartedAt = startedAt;
            localFinishedAt = finishedAt;
        }

        std::vector<UnstructuredMesh::Pointer> meshes;
        CollectUnstructuredMeshes(root, meshes);
        if (meshes.size() != 1u) {
            if (error != nullptr) {
                *error = "prepared surface requires exactly one unstructured mesh";
            }
            return false;
        }

        auto surface = SurfaceMesh::New();
        FlatArray<igIndex>::Pointer pointMap;
        std::shared_ptr<std::vector<igIndex>> faceToCellMap;
        const auto finalizeStart = std::chrono::steady_clock::now();
        if (!localBuilder->Finalize(
                meshes.front(),
                surface,
                pointMap,
                faceToCellMap,
                error)) {
            return false;
        }
        meshes.front()->SetPreparedSurfaceMesh(
            surface,
            std::move(pointMap),
            std::move(faceToCellMap));
        const auto finalizeMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - finalizeStart).count();
        const auto overlapMs = std::chrono::duration<double, std::milli>(
            localFinishedAt - localStartedAt).count();

        try {
        std::ostringstream output;
        output << std::fixed << std::setprecision(2)
               << "surface-execution=codec-workers"
               << "; surface-worker-capacity=" << workerCapacity
               << "; surface-peak-concurrency=" << peakActiveBlocks
               << "; surface-blocks=" << completedBlockCount
               << "/" << expectedBlockCount
               << "; surface-overlap=" << overlapMs << " ms"
               << "; surface-accumulate-task-ms=" << localAccumulateCpuMs
               << "; surface-finalize=" << finalizeMs << " ms"
               << "; fixed-cell-size=" << fixedCellSize
               << "; has-offsets=" << (hasOffsets ? 1 : 0)
               << "; has-cell-types=" << (hasCellTypes ? 1 : 0)
               << "; surface-points=" << surface->GetNumberOfPoints()
               << "; surface-faces=" << surface->GetNumberOfFaces();
        {
            std::lock_guard<std::mutex> lock(mutex);
            summary = output.str();
        }
        } catch (...) {
            diagnosticsIncomplete.store(true, std::memory_order_relaxed);
        }
        return true;
    }

    mutable std::mutex mutex;
    bool completed{false};
    bool failed{false};
    std::size_t expectedBlockCount{0u};
    std::size_t completedBlockCount{0u};
    std::size_t expectedCellCount{0u};
    std::size_t completedCellCount{0u};
    std::size_t activeBlocks{0u};
    std::size_t peakActiveBlocks{0u};
    std::size_t workerCapacity{1u};
    std::vector<BlockProgress> blockProgress;
    std::shared_ptr<ModelGeometryDecodedSurfaceBuilder> builder;
    std::atomic_bool diagnosticsIncomplete{false};
    double accumulateCpuMs{0.0};
    int fixedCellSize{0};
    bool hasOffsets{false};
    bool hasCellTypes{false};
    std::chrono::steady_clock::time_point startedAt{};
    std::chrono::steady_clock::time_point finishedAt{};
    std::string summary;
};

iGamePreparedSurfaceDecodeAdapter::iGamePreparedSurfaceDecodeAdapter()
    : m_impl(std::make_unique<Impl>()) {}

iGamePreparedSurfaceDecodeAdapter::~iGamePreparedSurfaceDecodeAdapter() = default;

bool iGamePreparedSurfaceDecodeAdapter::BeginConnectivityTopology(
    const ::datacodec::ConnectivityTopologyDecodeInfo& info,
    std::string* error) {
    return m_impl->BeginSurface(info, error);
}

bool iGamePreparedSurfaceDecodeAdapter::ObserveConnectivityBlock(
    ::datacodec::DecodedConnectivityTopologyBlock block,
    std::string* error) {
    return m_impl->AccumulateSurfaceBlock(std::move(block), error);
}

bool iGamePreparedSurfaceDecodeAdapter::EndConnectivityTopology(std::string* error) {
    return m_impl->EndSurface(error);
}

bool iGamePreparedSurfaceDecodeAdapter::AttachPreparedSurface(
    const DataObject::Pointer& root,
    std::string* error) {
    return m_impl->AttachPreparedSurface(root, error);
}

std::string iGamePreparedSurfaceDecodeAdapter::Summary() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->summary;
}

bool iGamePreparedSurfaceDecodeAdapter::DiagnosticsIncomplete() const noexcept {
    return m_impl->diagnosticsIncomplete.load(std::memory_order_relaxed);
}

IGAME_NAMESPACE_END
