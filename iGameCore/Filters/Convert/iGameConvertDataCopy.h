#ifndef iGameConvertDataCopy_h
#define iGameConvertDataCopy_h

#include "iGameCellArray.h"
#include "iGameDataObject.h"
#include "iGamePointSet.h"
#include "iGamePoints.h"
#include "iGameSurfaceMesh.h"
#include "iGameUnstructuredMesh.h"
#include "iGameVolumeMesh.h"

IGAME_NAMESPACE_BEGIN
namespace ConvertUtil {

/**
 * 为转换类过滤器创建独立的数据对象副本。
 *
 * 说明：
 * - 几何/拓扑（Points、Cells、Faces、Volumes、CellTypes 等）做深拷贝；
 * - 属性集不在本函数处理，由调用方自行深拷贝并转换；
 * - 返回 nullptr 表示当前类型不支持复制，调用方应直接返回失败。
 */
inline DataObject::Pointer CreateDataObjectCopy(DataObject::Pointer input) {
    if (input == nullptr) { return nullptr; }

    switch (input->GetDataObjectType()) {
        case IG_POINT_SET: {
            auto src = DynamicCast<PointSet>(input);
            if (src == nullptr || src->GetPoints() == nullptr) { return nullptr; }

            auto dst = PointSet::New();
            auto points = Points::New();
            if (!points->DeepCopy(src->GetPoints())) { return nullptr; }
            dst->SetPoints(points);
            return dst;
        }

        case IG_SURFACE_MESH: {
            auto src = DynamicCast<SurfaceMesh>(input);
            if (src == nullptr || src->GetPoints() == nullptr) { return nullptr; }

            auto dst = SurfaceMesh::New();
            auto points = Points::New();
            if (!points->DeepCopy(src->GetPoints())) { return nullptr; }
            dst->SetPoints(points);

            if (src->GetEdges() != nullptr) {
                auto edges = CellArray::New();
                if (!edges->DeepCopy(src->GetEdges())) { return nullptr; }
                dst->SetEdges(edges);
            }

            if (src->GetFaces() != nullptr) {
                auto faces = CellArray::New();
                if (!faces->DeepCopy(src->GetFaces())) { return nullptr; }
                dst->SetFaces(faces);
            }
            return dst;
        }

        case IG_VOLUME_MESH: {
            auto src = DynamicCast<VolumeMesh>(input);
            if (src == nullptr || src->GetPoints() == nullptr) { return nullptr; }

            auto dst = VolumeMesh::New();
            auto points = Points::New();
            if (!points->DeepCopy(src->GetPoints())) { return nullptr; }
            dst->SetPoints(points);

            if (src->GetEdges() != nullptr) {
                auto edges = CellArray::New();
                if (!edges->DeepCopy(src->GetEdges())) { return nullptr; }
                dst->SetEdges(edges);
            }

            if (src->GetFaces() != nullptr) {
                auto faces = CellArray::New();
                if (!faces->DeepCopy(src->GetFaces())) { return nullptr; }
                dst->SetFaces(faces);
            }

            dst->SetIsPolyhedronType(src->GetIsPolyhedronType());
            if (src->GetIsPolyhedronType()) {
                // 多面体体网格需要保留 volume -> face 映射，才能重建 VolumeMesh 的体单元。
                if (src->GetFaces() == nullptr) { return nullptr; }

                auto faces = CellArray::New();
                if (!faces->DeepCopy(src->GetFaces())) { return nullptr; }

                auto volumeFaces = CellArray::New();
                igIndex faceIds[IGAME_CELL_MAX_SIZE]{};
                for (IGsize i = 0; i < src->GetNumberOfVolumes(); ++i) {
                    int faceCount = src->GetVolumeFaceIds(i, faceIds);
                    if (faceCount > 0) { volumeFaces->AddCellIds(faceIds, faceCount); }
                }
                dst->InitVolumesWithPolyhedron(faces, volumeFaces);
            } else {
                if (src->GetVolumes() == nullptr) { return nullptr; }
                auto volumes = CellArray::New();
                if (!volumes->DeepCopy(src->GetVolumes())) { return nullptr; }
                dst->SetVolumes(volumes);
            }
            return dst;
        }

        case IG_UNSTRUCTURED_MESH: {
            auto src = DynamicCast<UnstructuredMesh>(input);
            if (src == nullptr || src->GetPoints() == nullptr ||
                src->GetCells() == nullptr) {
                return nullptr;
            }

            auto dst = UnstructuredMesh::New();
            auto points = Points::New();
            if (!points->DeepCopy(src->GetPoints())) { return nullptr; }
            dst->SetPoints(points);

            auto cells = CellArray::New();
            if (!cells->DeepCopy(src->GetCells())) { return nullptr; }

            if (src->GetCellTypes() != nullptr) {
                auto types = UnsignedIntArray::New();
                if (!types->DeepCopy(src->GetCellTypes())) { return nullptr; }
                dst->SetCells(cells, types);
            } else {
                dst->SetCells(cells, nullptr);
            }
            return dst;
        }

        default:
            return nullptr;
    }
}

} // namespace ConvertUtil
IGAME_NAMESPACE_END

#endif
