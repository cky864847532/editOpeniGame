#ifndef iGameCellTypeFilter_h
#define iGameCellTypeFilter_h

#include "iGameFilter.h"
#include "iGameUnstructuredMesh.h"

IGAME_NAMESPACE_BEGIN

class CellTypeFilter : public Filter {
public:
    I_OBJECT(CellTypeFilter);
    static Pointer New() { return new CellTypeFilter; }

    // 参数：只保留指定类型的单元（如 IG_TETRA = 9）
    void SetTargetCellType(igIndex type) { m_TargetType = type; }
    igIndex GetTargetCellType() const { return m_TargetType; }

    bool Execute() override;

protected:
    CellTypeFilter();
    ~CellTypeFilter() override = default;

    igIndex m_TargetType = IG_TETRA;
};

IGAME_NAMESPACE_END
#endif