#include "iGameConvertToCellDataFilter.h"
#include "iGameConvertDataCopy.h"
#include "iGameDataObject.h"

IGAME_NAMESPACE_BEGIN

bool ConvertToCellDataFilter::Execute() {
    auto Input = GetInput(0);
    if (Input == nullptr) { return false; }

    auto InputPoints = Input->GetPoints();
    auto InputCells = Input->GetCellArray();
    if (InputPoints == nullptr || InputCells == nullptr) { return false; }

    auto InputAttrs = Input->GetAttributeSet();
    if (InputAttrs == nullptr || InputAttrs->GetNumberOfAttributes() == 0) {
        return false;
    }

    bool HasPointAttribute = false;
    for (IGsize i = 0; i < InputAttrs->GetNumberOfAttributes(); ++i) {
        if (InputAttrs->GetAttribute(i).attachmentType == IG_POINT) {
            HasPointAttribute = true;
            break;
        }
    }
    if (!HasPointAttribute) { return false; }

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
    Output->SetName(BaseName + "_CellData");
    // 保持与原模型相同的当前属性索引（属性顺序不变，转换后仍可显示同一字段）。
    Output->SetAttributeIndex(Input->GetAttributeIndex());

    auto Points = Output->GetPoints();
    auto Cells = Output->GetCellArray();
    auto Attrs = Output->GetAttributeSet();
    if (Points == nullptr || Cells == nullptr || Attrs == nullptr) {
        return false;
    }

    igIndex ids[IGAME_CELL_MAX_SIZE]{};
    float vals[IGAME_CELL_MAX_SIZE]{};

    // 点数据 -> 单元数据：每个单元取它包含的点的算术平均。
    for (IGsize i = 0; i < Attrs->GetNumberOfAttributes(); ++i) {
        auto& attr = Attrs->GetAttribute(i);
        if (attr.attachmentType != IG_POINT || attr.pointer == nullptr) {
            continue;
        }

        const int dim = attr.pointer->GetDimension();
        if (dim <= 0 || dim > IGAME_CELL_MAX_SIZE) { continue; }

        FloatArray::Pointer arr = FloatArray::New();
        arr->SetDimension(dim);
        arr->SetName(attr.pointer->GetName());
        arr->Resize(Cells->GetNumberOfCells());

        for (IGsize j = 0; j < Cells->GetNumberOfCells(); ++j) {
            int Size = Cells->GetCellIds(j, ids);
            if (Size <= 0) { continue; }

            for (int k = 0; k < Size; ++k) {
                const igIndex pid = ids[k];
                if (pid < 0 ||
                    pid >= static_cast<igIndex>(Points->GetNumberOfPoints())) {
                    continue;
                }

                attr.pointer->GetElement(pid, vals);
                for (int d = 0; d < dim; ++d) {
                    arr->SetValue(j * dim + d,
                                  arr->GetValue(j * dim + d) +
                                          vals[d] / static_cast<float>(Size));
                }
            }
        }

        attr.pointer = arr;
        attr.attachmentType = IG_CELL;
        attr.UpdateAllDataRange();
    }

    SetOutput(Output);
    return true;
}

ConvertToCellDataFilter::ConvertToCellDataFilter() {
    SetNumberOfInputs(1);
    SetNumberOfOutputs(1);
}
IGAME_NAMESPACE_END
