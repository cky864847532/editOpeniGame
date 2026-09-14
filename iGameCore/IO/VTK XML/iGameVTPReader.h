/**
 * @class   iGameVTPReader
 * @brief   VTK XML PolyData reader for VTP files.
 */

#pragma once

#include "iGameVTUReader.h"

namespace tinyxml2 {
class XMLElement;
}

IGAME_NAMESPACE_BEGIN

class iGameVTPReader : public iGameVTUReader {
public:
    I_OBJECT(iGameVTPReader);

    static Pointer New() { return new iGameVTPReader; }

    bool Execute() override;
    bool Parsing() override;
    bool CreateDataObject() override;

protected:
    iGameVTPReader() = default;
    ~iGameVTPReader() override = default;

private:
    // -1: layout belongs to the general XML reader; 0: invalid input; 1: success.
    int ReadRawAppendedTriangles();
    bool ReadVTPPointData();
    bool ReadVTPPointAttribute();
    bool ReadVTPCellData();
    bool ReadPolyDataCells(const char* sectionName, bool asLines, bool asTriangleStrips);
    ArrayObject::Pointer ReadPolyDataIndexArray(tinyxml2::XMLElement* section, const char* arrayName, bool prependZero);
};

IGAME_NAMESPACE_END
