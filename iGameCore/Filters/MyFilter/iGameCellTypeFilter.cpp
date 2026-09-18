#include "iGameCellTypeFilter.h"

IGAME_NAMESPACE_BEGIN

CellTypeFilter::CellTypeFilter() {
    SetNumberOfInputs(1);
    SetNumberOfOutputs(1);
}

bool CellTypeFilter::Execute() {
    auto input = GetInput(0);
    if (!input) return false;
    igDebug("CellTypeFilter input type: {}", input->GetDataObjectType()); 
    if (input->GetDataObjectType() != IG_UNSTRUCTURED_MESH) return false;

    auto mesh = DynamicCast<UnstructuredMesh>(input);
    if (!mesh) return false;

    // 1. 收集要保留的单元
    auto inCells = mesh->GetCells();
    auto inTypes = mesh->GetCellTypes();
    igIndex inCellNum = mesh->GetNumberOfCells();

    std::vector<igIndex> keepCells;
    keepCells.reserve(inCellNum);
    for (igIndex i = 0; i < inCellNum; i++) {
        if (inTypes->GetValue(i) == m_TargetType) keepCells.push_back(i);
    }

    // 2. 构建新的连接表
    CellArray::Pointer newCells = CellArray::New();
    UnsignedIntArray::Pointer newTypes = UnsignedIntArray::New();
    igIndex ids[IGAME_CELL_MAX_SIZE] = {0};
    for (igIndex keepId: keepCells) {
        igIndex n = inCells->GetCellIds(keepId, ids);
        newCells->AddCellIds(ids, n);
        newTypes->AddValue(m_TargetType);
    }

    // 3. 输出网格：点全部保留，所以点属性可以直接共享
    auto outMesh = UnstructuredMesh::New();
    outMesh->SetName(mesh->GetName() + "_type" + std::to_string(m_TargetType));
    outMesh->SetPoints(mesh->GetPoints());
    outMesh->SetCells(newCells, newTypes);

    // 4. 属性复制：点属性原样带过去，单元属性按 keepCells 索引抽出来
    auto inAttr = mesh->GetAttributeSet();
    auto outAttr = AttributeSet::New();
    auto allAttrs = inAttr->GetAllAttributes();
    double values[IGAME_CELL_MAX_SIZE] = {0};
    for (igIndex i = 0; i < allAttrs->GetNumberOfElements(); i++) {
        auto attr = allAttrs->GetElement(i);
        if (attr.attachmentType == IG_POINT) {
            // 点没变，直接复用原数组
            outAttr->AddAttribute(attr.type, IG_POINT, attr.pointer, attr.GetDataRange());
        } else if (attr.attachmentType == IG_CELL) {
            auto outArray = FloatArray::New();
            outArray->SetName(attr.pointer->GetName());
            outArray->SetDimension(attr.pointer->GetDimension());
            outArray->Resize(keepCells.size());
            for (size_t j = 0; j < keepCells.size(); j++) {
                attr.pointer->GetElement(keepCells[j], values);
                outArray->SetElement(j, values);
            }
            outAttr->AddAttribute(attr.type, IG_CELL, outArray, attr.GetDataRange());
        }
    }
    outMesh->SetAttributeSet(outAttr);

    // 5. 挂到输出端口
    SetOutput(outMesh);
    return true;
}

IGAME_NAMESPACE_END