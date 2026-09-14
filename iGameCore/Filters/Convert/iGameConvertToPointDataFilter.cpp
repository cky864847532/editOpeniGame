#include "iGameConvertToPointDataFilter.h"
#include "iGameConvertDataCopy.h"
#include "iGameDataObject.h"

IGAME_NAMESPACE_BEGIN

bool ConvertToPointDataFilter::Execute() {
    auto Input = GetInput(0);
    if (Input == nullptr) { return false; }

    auto InputPoints = Input->GetPoints();
    auto InputCells = Input->GetCellArray();
    if (InputPoints == nullptr || InputCells == nullptr) { return false; }

    auto InputAttrs = Input->GetAttributeSet();
    if (InputAttrs == nullptr || InputAttrs->GetNumberOfAttributes() == 0) {
        return false;
    }

    bool HasCellAttribute = false;
    for (IGsize i = 0; i < InputAttrs->GetNumberOfAttributes(); ++i) {
        if (InputAttrs->GetAttribute(i).attachmentType == IG_CELL) {
            HasCellAttribute = true;
            break;
        }
    }
    if (!HasCellAttribute) { return false; }

    // 创建一个独立的数据对象副本，原模型保持不变。
    auto Output = ConvertUtil::CreateDataObjectCopy(Input);
    if (Output == nullptr) { return false; }

    // 属性集深拷贝到新对象，后续转换只修改副本。
    auto OutputAttrs = AttributeSet::New();
    if (!OutputAttrs->DeepCopy(InputAttrs)) { return false; }
    Output->SetAttributeSet(OutputAttrs);

    std::string BaseName = Input->GetName();
    if (BaseName.empty()) {
        BaseName = "Block_" + std::to_string(Input->GetDataObjectId());
    }
    Output->SetName(BaseName + "_PointData");
    // 保持与原模型相同的当前属性索引（属性顺序不变，转换后仍可显示同一字段）。
    Output->SetAttributeIndex(Input->GetAttributeIndex());

    auto Points = Output->GetPoints();
    auto Cells = Output->GetCellArray();
    auto Attrs = Output->GetAttributeSet();
    if (Points == nullptr || Cells == nullptr || Attrs == nullptr) {
        return false;
    }

    // 统计每个点被多少个单元包含。
    std::vector<unsigned int> PointDegree(Points->GetNumberOfPoints(), 0);

    igIndex ids[IGAME_CELL_MAX_SIZE]{};
    float vals[IGAME_CELL_MAX_SIZE]{};

    for (IGsize i = 0; i < Cells->GetNumberOfCells(); ++i) {
        int Size = Cells->GetCellIds(i, ids);
        for (int j = 0; j < Size; ++j) {
            if (ids[j] < static_cast<igIndex>(PointDegree.size())) {
                PointDegree[ids[j]]++;
            }
        }
    }

    // 单元数据 -> 点数据：每个点取它所关联单元值的算术平均。
    for (IGsize i = 0; i < Attrs->GetNumberOfAttributes(); ++i) {
        auto& attr = Attrs->GetAttribute(i);
        if (attr.attachmentType != IG_CELL || attr.pointer == nullptr) {
            continue;
        }

        const int dim = attr.pointer->GetDimension();
        if (dim <= 0 || dim > IGAME_CELL_MAX_SIZE) { continue; }

        FloatArray::Pointer arr = FloatArray::New();
        arr->SetDimension(dim);
        arr->SetName(attr.pointer->GetName());
        arr->Resize(Points->GetNumberOfPoints());

        for (IGsize j = 0; j < Cells->GetNumberOfCells(); ++j) {
            int Size = Cells->GetCellIds(j, ids);
            if (Size <= 0) { continue; }

            attr.pointer->GetElement(j, vals);
            for (int k = 0; k < Size; ++k) {
                const igIndex pid = ids[k];
                if (pid >= static_cast<igIndex>(PointDegree.size()) ||
                    PointDegree[pid] == 0) {
                    continue;
                }

                for (int d = 0; d < dim; ++d) {
                    arr->SetValue(
                            pid * dim + d,
                            arr->GetValue(pid * dim + d) +
                                    vals[d] /
                                            static_cast<float>(PointDegree[pid]));
                }
            }
        }

        attr.pointer = arr;
        attr.attachmentType = IG_POINT;
        attr.UpdateAllDataRange();
    }

    SetOutput(Output);
    return true;
}

ConvertToPointDataFilter::ConvertToPointDataFilter() {
    SetNumberOfInputs(1);
    SetNumberOfOutputs(1);
}
IGAME_NAMESPACE_END
