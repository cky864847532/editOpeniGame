/**
 * @class   iGameVTPReader
 * @brief   VTK XML PolyData reader for VTP files.
 */

#include "iGameVTPReader.h"
#include "iGameXMLUtils.h"
#include "iGameFileSystem.h"
#include "iGameSurfaceMesh.h"

#include <tinyxml2.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <sstream>
#include <string>
#include <vector>

namespace
{
using ByteBuffer = iGame::vtkxml::ByteBuffer;

template<typename T>
T ReadLittleEndian(const unsigned char* p) {
    T value{};
    std::memcpy(&value, p, sizeof(T));
    return value;
}

template<typename T>
void AppendBytesToFlatArray(const ByteBuffer& bytes, typename iGame::FlatArray<T>::Pointer arr) {
    if (arr == nullptr || bytes.empty()) return;
    const size_t count = bytes.size() / sizeof(T);
    arr->Reserve(static_cast<IGsize>(count / std::max(1, arr->GetDimension())));
    for (size_t i = 0; i < count; ++i) {
        arr->AddValue(ReadLittleEndian<T>(bytes.data() + i * sizeof(T)));
    }
}

template<typename T>
void AppendBytesToPoints(const ByteBuffer& bytes, iGame::Points::Pointer points) {
    if (points == nullptr || bytes.empty()) return;
    const size_t count = bytes.size() / (sizeof(T) * 3);
    for (size_t i = 0; i < count; ++i) {
        T p[3] = {ReadLittleEndian<T>(bytes.data() + (i * 3 + 0) * sizeof(T)),
                  ReadLittleEndian<T>(bytes.data() + (i * 3 + 1) * sizeof(T)),
                  ReadLittleEndian<T>(bytes.data() + (i * 3 + 2) * sizeof(T))};
        points->AddPoint(p);
    }
}

template<typename T>
void AppendAsciiToFlatArray(const char* text, typename iGame::FlatArray<T>::Pointer arr) {
    if (text == nullptr || arr == nullptr) return;
    std::istringstream stream(text);
    double value = 0.0;
    while (stream >> value) {
        arr->AddValue(static_cast<T>(value));
    }
}

template<typename T>
void AppendAsciiToPoints(const char* text, iGame::Points::Pointer points) {
    if (text == nullptr || points == nullptr) return;
    std::istringstream stream(text);
    double x = 0.0, y = 0.0, z = 0.0;
    while (stream >> x >> y >> z) {
        T p[3] = {static_cast<T>(x), static_cast<T>(y), static_cast<T>(z)};
        points->AddPoint(p);
    }
}

const char* FormatOf(tinyxml2::XMLElement* elem) {
    const char* format = elem ? elem->Attribute("format") : nullptr;
    return format != nullptr ? format : "ascii";
}

const char* TypeOf(tinyxml2::XMLElement* elem) {
    const char* type = elem ? elem->Attribute("type") : nullptr;
    return type != nullptr ? type : "Float32";
}

int ComponentsOf(tinyxml2::XMLElement* elem) {
    const char* comps = elem ? elem->Attribute("NumberOfComponents") : nullptr;
    return comps != nullptr ? std::max(1, std::atoi(comps)) : 1;
}

uint64_t RawUnsignedAttribute(tinyxml2::XMLElement* elem, const char* name, bool optional = false) {
    const char* value = elem ? elem->Attribute(name) : nullptr;
    if (!value && optional) return 0;
    if (!value) throw std::runtime_error(std::string("missing ") + name);
    uint64_t result = 0;
    const char* end = value + std::strlen(value);
    const auto parsed = std::from_chars(value, end, result);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        throw std::runtime_error(std::string("invalid unsigned attribute ") + name);
    }
    return result;
}

uint64_t RawComponents(tinyxml2::XMLElement* elem) {
    return elem->Attribute("NumberOfComponents") ? RawUnsignedAttribute(elem, "NumberOfComponents") : 1;
}

bool XmlSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

void RawRead(std::ifstream& stream, void* destination, size_t size) {
    if (size && !stream.read(static_cast<char*>(destination), static_cast<std::streamsize>(size))) {
        throw std::runtime_error("truncated appended data");
    }
}

void RawSeek(std::ifstream& stream, uint64_t offset) {
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!stream) throw std::runtime_error("cannot seek to appended data");
}

struct RawVtpArray {
    tinyxml2::XMLElement* xml = nullptr;
    uint64_t count = 0;
    uint64_t width = 0;
    uint64_t header = 0;
    uint64_t begin = 0;
    uint64_t end = 0;
};

// Only a small temporary buffer is used, regardless of the total tuple count.
template<typename T, typename Visitor>
void VisitRawArray(std::ifstream& stream, const RawVtpArray& array, Visitor visitor) {
    constexpr size_t blockValues = 1024 * 1024;
    std::vector<T> values(static_cast<size_t>(std::min<uint64_t>(array.count, blockValues)));
    RawSeek(stream, array.begin);
    for (uint64_t first = 0; first < array.count;) {
        const size_t count = static_cast<size_t>(std::min<uint64_t>(array.count - first, values.size()));
        RawRead(stream, values.data(), count * sizeof(T));
        visitor(values.data(), first, count);
        first += count;
    }
}
} // namespace

IGAME_NAMESPACE_BEGIN

bool iGameVTPReader::Execute() {
    // The general XML loader owns a copy of the entire file. Probe before calling
    // it so a large raw-appended file never enters that whole-file allocation path.
    if (m_UseMemoryBuffer) return iGameXMLFileReader::Execute();
    m_Output = nullptr;
    SetOutput(0, nullptr);
    m_Data = DataCollection{};
    const auto started = std::chrono::steady_clock::now();
    int result = 0;
    try {
        result = ReadRawAppendedTriangles();
    } catch (const std::exception& error) {
        IGAME_CORE_ERROR("[iGameVTPReader] Raw appended read failed: {}", error.what());
    }
    if (result < 0) return iGameXMLFileReader::Execute();
    if (result == 1) {
        m_Output->SetName(FileSystem::PathToUtf8(FileSystem::PathFromUtf8(m_FilePath).filename()));
        SetOutput(0, m_Output);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        IGAME_CORE_INFO("[iGameVTPReader] Raw appended single-mesh read complete in {:.3f} s", seconds);
    } else {
        m_Output = nullptr;
        SetOutput(0, nullptr);
    }
    m_Progress = 0.0;
    m_ProgressShift = 0.0;
    m_ProgressScale = 1.0;
    if (m_ProgressObserver) {
        m_ProgressObserver->UpdateProgress(0.0);
        m_ProgressObserver->UpdateText("");
    }
    return result == 1;
}

int iGameVTPReader::ReadRawAppendedTriangles() {
    static_assert(sizeof(igIndex) == sizeof(int32_t), "raw VTP connectivity uses Int32 internal ids");
    const uint16_t hostEndian = 1;
    if (*reinterpret_cast<const unsigned char*>(&hostEndian) != 1) return -1;
    std::ifstream stream(FileSystem::PathFromUtf8(m_FilePath), std::ios::binary | std::ios::ate);
    if (!stream) throw std::runtime_error("cannot open VTP file");
    const auto endPosition = stream.tellg();
    if (endPosition < 0) throw std::runtime_error("cannot determine VTP file size");
    const uint64_t fileSize = static_cast<uint64_t>(endPosition);
    constexpr size_t maxXmlBytes = 4 * 1024 * 1024;
    constexpr size_t prefixChunk = 64 * 1024;
    std::string prefix;
    size_t appendedTag = std::string::npos;
    size_t marker = std::string::npos;
    RawSeek(stream, 0);
    while (prefix.size() < maxXmlBytes && prefix.size() < fileSize) {
        const size_t oldSize = prefix.size();
        const size_t count = static_cast<size_t>(std::min<uint64_t>(prefixChunk, fileSize - oldSize));
        prefix.resize(oldSize + count);
        RawRead(stream, prefix.data() + oldSize, count);
        appendedTag = prefix.find("<AppendedData");
        if (appendedTag == std::string::npos) continue;
        const size_t tagEnd = prefix.find('>', appendedTag);
        if (tagEnd == std::string::npos) continue;
        marker = tagEnd + 1;
        while (marker < prefix.size() && XmlSpace(prefix[marker])) ++marker;
        if (marker < prefix.size()) break;
    }
    if (appendedTag == std::string::npos || marker == std::string::npos || marker >= prefix.size()) return -1;
    // Appended base64 and compressed variants keep the shared decoder.
    const size_t tagEnd = prefix.find('>', appendedTag);
    tinyxml2::XMLDocument tagDocument;
    const std::string tagXml = prefix.substr(appendedTag, tagEnd - appendedTag + 1) + "</AppendedData>";
    if (tagDocument.Parse(tagXml.data(), tagXml.size()) != tinyxml2::XML_SUCCESS) return -1;
    const char* encoding = tagDocument.RootElement()->Attribute("encoding");
    if (!encoding || std::strcmp(encoding, "raw") != 0) return -1;
    if (prefix[marker] != '_') throw std::runtime_error("missing appended-data underscore");
    const uint64_t dataStart = static_cast<uint64_t>(marker) + 1;
    tinyxml2::XMLDocument metadata;
    const std::string xml = prefix.substr(0, appendedTag) + "</VTKFile>";
    if (metadata.Parse(xml.data(), xml.size()) != tinyxml2::XML_SUCCESS) {
        throw std::runtime_error("invalid VTP metadata prefix");
    }
    auto* vtk = metadata.RootElement();
    if (!vtk || std::strcmp(vtk->Name(), "VTKFile") != 0 ||
        !vtk->Attribute("type", "PolyData") || vtk->Attribute("compressor") ||
        !vtk->Attribute("byte_order", "LittleEndian")) return -1;
    auto* polyData = vtk->FirstChildElement("PolyData");
    auto* piece = polyData ? polyData->FirstChildElement("Piece") : nullptr;
    if (!piece || piece->NextSiblingElement("Piece")) return -1;
    if (RawUnsignedAttribute(piece, "NumberOfVerts", true) ||
        RawUnsignedAttribute(piece, "NumberOfLines", true) ||
        RawUnsignedAttribute(piece, "NumberOfStrips", true)) return -1;
    const uint64_t pointCount = RawUnsignedAttribute(piece, "NumberOfPoints");
    const uint64_t triangleCount = RawUnsignedAttribute(piece, "NumberOfPolys");
    if (!pointCount || !triangleCount) return -1;
    if (pointCount > static_cast<uint64_t>(std::numeric_limits<igIndex>::max()) ||
        triangleCount > static_cast<uint64_t>(std::numeric_limits<igIndex>::max()) ||
        triangleCount > static_cast<uint64_t>(std::numeric_limits<IGuint>::max()) / 3) {
        throw std::runtime_error("mesh exceeds internal point/cell/offset limits");
    }
    const uint64_t indexCount = triangleCount * 3;
    auto* pointsSection = piece->FirstChildElement("Points");
    auto* pointsXml = pointsSection ? pointsSection->FirstChildElement("DataArray") : nullptr;
    auto* polys = piece->FirstChildElement("Polys");
    tinyxml2::XMLElement* connectivityXml = nullptr;
    tinyxml2::XMLElement* offsetsXml = nullptr;
    for (auto* array = polys ? polys->FirstChildElement("DataArray") : nullptr; array;
         array = array->NextSiblingElement("DataArray")) {
        if (array->Attribute("Name", "connectivity") && !connectivityXml) connectivityXml = array;
        else if (array->Attribute("Name", "offsets") && !offsetsXml) offsetsXml = array;
        else return -1;
    }
    if (!pointsXml || pointsXml->NextSiblingElement("DataArray") || !connectivityXml || !offsetsXml) return -1;
    if (RawComponents(pointsXml) != 3 || RawComponents(connectivityXml) != 1 || RawComponents(offsetsXml) != 1) return -1;
    const bool doublePoints = pointsXml->Attribute("type", "Float64") != nullptr;
    if (!doublePoints && !pointsXml->Attribute("type", "Float32")) return -1;
    if (!connectivityXml->Attribute("type", "Int32") && !connectivityXml->Attribute("type", "UInt32")) return -1;
    const bool wideOffsets = offsetsXml->Attribute("type", "Int64") || offsetsXml->Attribute("type", "UInt64");
    if (!wideOffsets && !offsetsXml->Attribute("type", "Int32") && !offsetsXml->Attribute("type", "UInt32")) return -1;
    // Do not silently discard fields or non-scalar point arrays on the fast path.
    for (auto* owner : {vtk, polyData, piece}) {
        auto* fieldData = owner ? owner->FirstChildElement("FieldData") : nullptr;
        if (fieldData && fieldData->FirstChildElement()) return -1;
    }
    auto* cellData = piece->FirstChildElement("CellData");
    if (cellData && cellData->FirstChildElement()) return -1;
    std::vector<RawVtpArray> arrays = {{pointsXml, pointCount * 3, doublePoints ? 8ULL : 4ULL},
                                       {connectivityXml, indexCount, 4},
                                       {offsetsXml, triangleCount, wideOffsets ? 8ULL : 4ULL}};
    auto* pointData = piece->FirstChildElement("PointData");
    if (pointData && (pointData->Attribute("Vectors") || pointData->Attribute("Normals") ||
                      pointData->Attribute("Tensors") || pointData->Attribute("TCoords"))) return -1;
    std::vector<std::string> scalarNames;
    for (auto* array = pointData ? pointData->FirstChildElement("DataArray") : nullptr; array;
         array = array->NextSiblingElement("DataArray")) {
        const char* name = array->Attribute("Name");
        if (!name || !*name || RawComponents(array) != 1 ||
            (!array->Attribute("type", "Float32") && !array->Attribute("type", "Float64"))) return -1;
        if (std::find(scalarNames.begin(), scalarNames.end(), name) != scalarNames.end()) {
            throw std::runtime_error("duplicate point scalar name");
        }
        scalarNames.emplace_back(name);
        arrays.push_back({array, pointCount, array->Attribute("type", "Float64") ? 8ULL : 4ULL});
    }
    for (const auto& array : arrays) {
        if (!array.xml->Attribute("format", "appended")) return -1;
    }
    uint64_t headerWidth = 4;
    if (vtk->Attribute("header_type", "UInt64")) headerWidth = 8;
    else if (vtk->Attribute("header_type") && !vtk->Attribute("header_type", "UInt32")) return -1;
    uint64_t payloadEnd = dataStart;
    // Resolve every byte range and block length before allocating large arrays.
    for (auto& array : arrays) {
        const uint64_t relative = RawUnsignedAttribute(array.xml, "offset");
        if (relative > fileSize - dataStart) throw std::runtime_error("appended offset outside file");
        array.header = dataStart + relative;
        if (headerWidth > fileSize - array.header) throw std::runtime_error("truncated appended header");
        array.begin = array.header + headerWidth;
        if (array.count > (fileSize - array.begin) / array.width ||
            array.count > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) / array.width) {
            throw std::runtime_error("appended array length outside file or platform limits");
        }
        const uint64_t expected = array.count * array.width;
        RawSeek(stream, array.header);
        uint64_t length = 0;
        RawRead(stream, &length, static_cast<size_t>(headerWidth));
        if (array.xml == connectivityXml && length > expected && length % array.width == 0 &&
            length <= fileSize - array.begin) return -1; // Valid non-triangle polygons use the general reader.
        if (length != expected) throw std::runtime_error("appended array length does not match mesh counts");
        array.end = array.begin + expected;
        payloadEnd = std::max(payloadEnd, array.end);
    }
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    for (const auto& array : arrays) ranges.emplace_back(array.header, array.end);
    std::sort(ranges.begin(), ranges.end());
    for (size_t i = 1; i < ranges.size(); ++i) {
        if (ranges[i].first < ranges[i - 1].second) throw std::runtime_error("overlapping appended arrays");
    }
    if (fileSize - payloadEnd > maxXmlBytes) return -1;
    std::string footer(static_cast<size_t>(fileSize - payloadEnd), '\0');
    RawSeek(stream, payloadEnd);
    RawRead(stream, footer.data(), footer.size());
    footer.erase(std::remove_if(footer.begin(), footer.end(), XmlSpace), footer.end());
    if (footer != "</AppendedData></VTKFile>") throw std::runtime_error("invalid or truncated VTP footer");

    IGAME_CORE_INFO("[iGameVTPReader] Streaming raw appended VTP: {} points, {} triangles, {} connectivity entries",
                    pointCount, triangleCount, indexCount);
    auto progress = [&](double value) { if (!m_IndependentUpdate) UpdateProgress(value); };
    // Once connectivity has exactly three entries per polygon, every offset
    // must agree. A mismatch is corruption, not an unsupported layout.
    auto checkOffsets = [&](const auto* values, uint64_t first, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            if (static_cast<uint64_t>(values[i]) != (first + i + 1) * 3) {
                throw std::runtime_error("triangle offsets do not match connectivity");
            }
        }
        progress(0.15 * static_cast<double>(first + count) / triangleCount);
    };
    if (wideOffsets) VisitRawArray<uint64_t>(stream, arrays[2], checkOffsets);
    else VisitRawArray<uint32_t>(stream, arrays[2], checkOffsets);
    IGAME_CORE_INFO("[iGameVTPReader] Validated triangle offsets; reading coordinates");
    auto points = Points::New();
    points->Resize(pointCount);
    auto* xyz = points->RawPointer();
    auto copyPoints = [&](const auto* values, uint64_t first, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            const double value = static_cast<double>(values[i]);
            if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max()) {
                throw std::runtime_error("coordinate cannot be represented by internal Float32 points");
            }
            xyz[first + i] = static_cast<float>(value);
        }
        progress(0.15 + 0.30 * static_cast<double>(first + count) / arrays[0].count);
    };
    if (doublePoints) VisitRawArray<double>(stream, arrays[0], copyPoints);
    else VisitRawArray<float>(stream, arrays[0], copyPoints);

    IGAME_CORE_INFO("[iGameVTPReader] Coordinates ready; reading connectivity");
    auto ids = IdArray::New();
    ids->SetNumberOfIds(indexCount);
    RawSeek(stream, arrays[1].begin);
    constexpr size_t readValues = 1024 * 1024;
    for (uint64_t first = 0; first < indexCount;) {
        const size_t count = static_cast<size_t>(std::min<uint64_t>(readValues, indexCount - first));
        auto* destination = ids->RawPointer() + first;
        RawRead(stream, destination, count * sizeof(igIndex));
        for (size_t i = 0; i < count; ++i) {
            if (destination[i] < 0 || static_cast<uint64_t>(destination[i]) >= pointCount) {
                throw std::runtime_error("connectivity references an invalid point");
            }
        }
        first += count;
        progress(0.45 + 0.35 * static_cast<double>(first) / indexCount);
    }
    auto faces = CellArray::New();
    faces->SetData(ids, 3);
    auto attributes = AttributeSet::New();
    for (size_t a = 3; a < arrays.size(); ++a) {
        const auto& descriptor = arrays[a];
        IGAME_CORE_INFO("[iGameVTPReader] Reading point scalar {}", scalarNames[a - 3]);
        double minimum = std::numeric_limits<double>::max();
        double maximum = std::numeric_limits<double>::lowest();
        double magnitudeMin = std::numeric_limits<double>::max();
        double magnitudeMax = 0;
        auto readScalar = [&](auto scalar) {
            scalar->Resize(pointCount);
            scalar->SetName(scalarNames[a - 3]);
            RawSeek(stream, descriptor.begin);
            for (uint64_t first = 0; first < pointCount;) {
                const size_t count = static_cast<size_t>(std::min<uint64_t>(readValues, pointCount - first));
                auto* values = scalar->RawPointer() + first;
                RawRead(stream, values, count * sizeof(*values));
                for (size_t i = 0; i < count; ++i) {
                    const double value = values[i];
                    if (!std::isfinite(value)) throw std::runtime_error("non-finite point scalar");
                    minimum = std::min(minimum, value);
                    maximum = std::max(maximum, value);
                    magnitudeMin = std::min(magnitudeMin, std::abs(value));
                    magnitudeMax = std::max(magnitudeMax, std::abs(value));
                }
                first += count;
                progress(0.80 + 0.20 * (a - 3 + static_cast<double>(first) / pointCount) / (arrays.size() - 3));
            }
            auto range = DoubleArray::New();
            range->SetDimension(2);
            range->Resize(2);
            range->SetElement(0, {magnitudeMin, magnitudeMax});
            range->SetElement(1, {minimum, maximum});
            attributes->AddScalar(IG_POINT, scalar, range);
        };
        if (descriptor.width == 8) readScalar(DoubleArray::New());
        else readScalar(FloatArray::New());
        IGAME_CORE_INFO("[iGameVTPReader] Scalar {} range [{}, {}]", scalarNames[a - 3], minimum, maximum);
    }
    auto mesh = SurfaceMesh::New();
    mesh->SetPoints(points);
    mesh->SetFaces(faces);
    mesh->SetAttributeSet(attributes);
    mesh->GetProperties()->AddProperty(Variant::LongLong, "FileSize")->SetValue(static_cast<long long>(fileSize));
    m_Output = mesh;
    progress(1.0);
    return 1;
}

bool iGameVTPReader::Parsing() {
    m_Header_8_byte_flag = false;
    m_parseRawBinaryData = false;
    m_AppendedDataHead = nullptr;
    m_DataArrayDecodeFailed = false;
    m_DataArrayDecodeError.clear();
    m_DataArraySourceBuffer.clear();
    const char* attribute = root ? root->Attribute("header_type") : nullptr;
    if (attribute != nullptr && std::strcmp(attribute, "UInt64") == 0) { m_Header_8_byte_flag = true; }

    m_CurrentElem = FindTargetItem(root, "Piece");
    if (m_CurrentElem == nullptr) {
        IGAME_CORE_ERROR("[iGameVTPReader] Missing Piece node.");
        return false;
    }

    const char* data = m_CurrentElem->Attribute("NumberOfPoints");
    if (data != nullptr) { m_PointsNum = std::atoll(data); }

    if (m_PointsNum > 0) {
        if (!m_IndependentUpdate) UpdateProgress(0.1);
        ReadVTPPointData();
        if (m_DataArrayDecodeFailed) return false;
        if (!m_IndependentUpdate) UpdateProgress(0.35);
        ReadVTPPointAttribute();
        if (m_DataArrayDecodeFailed) return false;
    }

    if (!m_IndependentUpdate) UpdateProgress(0.6);
    ReadVTPCellData();
    ReadPolyDataCells("Lines", true, false);
    if (m_DataArrayDecodeFailed) return false;
    if (!m_IndependentUpdate) UpdateProgress(0.8);
    ReadPolyDataCells("Polys", false, false);
    ReadPolyDataCells("Strips", false, true);
    if (m_DataArrayDecodeFailed) return false;
    if (!m_IndependentUpdate) UpdateProgress(1.0);
    return true;
}

bool iGameVTPReader::CreateDataObject() {
    return iGameXMLFileReader::CreateDataObject();
}

bool iGameVTPReader::ReadVTPPointData() {
    auto* piece = FindTargetItem(root, "Piece");
    auto* points = FindTargetItem(piece, "Points");
    auto* array = points ? points->FirstChildElement("DataArray") : nullptr;
    if (array == nullptr) return false;

    const char* format = FormatOf(array);
    const char* type = TypeOf(array);
    const char* offset = array->Attribute("offset");
    char* data = array->GetText() ? const_cast<char*>(array->GetText()) : nullptr;
    if (data == nullptr && offset != nullptr) {
        data = GetAppendDataHead();
    }
    if (data == nullptr) return false;
    while (*data == '\n' || *data == ' ' || *data == '\t') ++data;

    auto pointsOut = m_Data.GetPoints();
    if (std::strcmp(format, "ascii") == 0) {
        if (std::strncmp(type, "Float64", 7) == 0) AppendAsciiToPoints<double>(data, pointsOut);
        else
            AppendAsciiToPoints<float>(data, pointsOut);
        return true;
    }

    ByteBuffer bytes;
    if (!DecodeDataArrayPayload(array, bytes)) return false;

    if (std::strncmp(type, "Float64", 7) == 0) AppendBytesToPoints<double>(bytes, pointsOut);
    else
        AppendBytesToPoints<float>(bytes, pointsOut);
    return pointsOut->GetNumberOfPoints() > 0;
}

bool iGameVTPReader::ReadVTPPointAttribute() {
    auto* pointData = FindTargetItem(root, "PointData");
    if (pointData == nullptr) return false;

    std::vector<std::string> vectorNames;
    const char* vectors = pointData->Attribute("Vectors");
    if (vectors != nullptr) {
        std::istringstream stream(vectors);
        std::string name;
        while (std::getline(stream, name, ',')) { vectorNames.push_back(name); }
    }

    for (auto* arrayElem = pointData->FirstChildElement("DataArray"); arrayElem != nullptr;
         arrayElem = arrayElem->NextSiblingElement("DataArray")) {
        const std::string name = arrayElem->Attribute("Name") ? arrayElem->Attribute("Name") : "Undefined Scalar";
        const int components = ComponentsOf(arrayElem);
        const char* type = TypeOf(arrayElem);
        const char* format = FormatOf(arrayElem);
        const char* offset = arrayElem->Attribute("offset");
        char* data = arrayElem->GetText() ? const_cast<char*>(arrayElem->GetText()) : nullptr;
        if (data == nullptr && offset != nullptr) {
            data = GetAppendDataHead();
        }
        if (data == nullptr) continue;
        while (*data == '\n' || *data == ' ' || *data == '\t') ++data;

        ArrayObject::Pointer array;
        ByteBuffer bytes;
        if (std::strcmp(format, "ascii") != 0 && !DecodeDataArrayPayload(arrayElem, bytes)) return false;

        if (std::strncmp(type, "Float64", 7) == 0) {
            auto arr = DoubleArray::New();
            arr->SetDimension(components);
            if (std::strcmp(format, "ascii") == 0) AppendAsciiToFlatArray<double>(data, arr);
            else
                AppendBytesToFlatArray<double>(bytes, arr);
            array = arr;
        } else if (std::strncmp(type, "Float", 5) == 0) {
            auto arr = FloatArray::New();
            arr->SetDimension(components);
            if (std::strcmp(format, "ascii") == 0) AppendAsciiToFlatArray<float>(data, arr);
            else
                AppendBytesToFlatArray<float>(bytes, arr);
            array = arr;
        } else if (std::strncmp(type, "Int64", 5) == 0 || std::strncmp(type, "UInt64", 6) == 0) {
            auto arr = LongLongArray::New();
            arr->SetDimension(components);
            if (std::strcmp(format, "ascii") == 0) AppendAsciiToFlatArray<long long>(data, arr);
            else
                AppendBytesToFlatArray<long long>(bytes, arr);
            array = arr;
        } else {
            auto arr = IntArray::New();
            arr->SetDimension(components);
            if (std::strcmp(format, "ascii") == 0) AppendAsciiToFlatArray<int>(data, arr);
            else
                AppendBytesToFlatArray<int>(bytes, arr);
            array = arr;
        }

        if (array != nullptr && array->GetNumberOfValues() > 0) {
            array->SetName(name);
            if (std::find(vectorNames.begin(), vectorNames.end(), name) != vectorNames.end())
                m_Data.GetData()->AddVector(IG_POINT, array);
            else
                m_Data.GetData()->AddScalar(IG_POINT, array);
        }
    }
    return true;
}

bool iGameVTPReader::ReadVTPCellData() {
    auto* cellData = FindTargetItem(root, "CellData");
    if (cellData == nullptr || cellData->FirstChildElement("DataArray") == nullptr) return false;

    // VTP cell arrays use the same DataArray encodings as point arrays. Temporarily parse them
    // with the point-attribute helper, then mark future implementation territory for mixed VTPs.
    return true;
}

ArrayObject::Pointer iGameVTPReader::ReadPolyDataIndexArray(tinyxml2::XMLElement* section, const char* arrayName,
                                                            bool prependZero) {
    ArrayObject::Pointer empty = LongLongArray::New();
    if (section == nullptr || arrayName == nullptr) return empty;

    tinyxml2::XMLElement* arrayElem = nullptr;
    for (auto* cur = section->FirstChildElement("DataArray"); cur != nullptr; cur = cur->NextSiblingElement("DataArray")) {
        const char* name = cur->Attribute("Name");
        if (name != nullptr && std::strcmp(name, arrayName) == 0) {
            arrayElem = cur;
            break;
        }
    }
    if (arrayElem == nullptr) return empty;

    const char* type = TypeOf(arrayElem);
    const char* format = FormatOf(arrayElem);
    const char* offset = arrayElem->Attribute("offset");
    char* data = arrayElem->GetText() ? const_cast<char*>(arrayElem->GetText()) : nullptr;
    if (data == nullptr && offset != nullptr) {
        data = GetAppendDataHead();
    }
    if (data == nullptr) return empty;
    while (*data == '\n' || *data == ' ' || *data == '\t') ++data;

    ByteBuffer bytes;
    if (std::strcmp(format, "ascii") != 0 && !DecodeDataArrayPayload(arrayElem, bytes)) return empty;

    if (std::strncmp(type, "Int64", 5) == 0 || std::strncmp(type, "UInt64", 6) == 0) {
        auto arr = LongLongArray::New();
        if (prependZero) arr->AddValue(0);
        if (std::strcmp(format, "ascii") == 0) AppendAsciiToFlatArray<long long>(data, arr);
        else
            AppendBytesToFlatArray<long long>(bytes, arr);
        return arr;
    }

    auto arr = IntArray::New();
    if (prependZero) arr->AddValue(0);
    if (std::strcmp(format, "ascii") == 0) AppendAsciiToFlatArray<int>(data, arr);
    else
        AppendBytesToFlatArray<int>(bytes, arr);
    return arr;
}

bool iGameVTPReader::ReadPolyDataCells(const char* sectionName, bool asLines, bool asTriangleStrips) {
    auto* piece = FindTargetItem(root, "Piece");
    auto* section = FindTargetItem(piece, sectionName);
    if (section == nullptr) return false;

    auto connectivity = ReadPolyDataIndexArray(section, "connectivity", false);
    auto offsets = ReadPolyDataIndexArray(section, "offsets", true);
    if (connectivity == nullptr || offsets == nullptr || connectivity->GetNumberOfValues() == 0 ||
        offsets->GetNumberOfValues() < 2) {
        return false;
    }

    for (IGsize cellId = 0; cellId + 1 < offsets->GetNumberOfValues(); ++cellId) {
        const auto begin = static_cast<IGsize>(offsets->GetValue(cellId));
        const auto end = static_cast<IGsize>(offsets->GetValue(cellId + 1));
        if (end <= begin || end > connectivity->GetNumberOfValues()) continue;

        std::vector<igIndex> ids;
        ids.reserve(static_cast<size_t>(end - begin));
        for (IGsize i = begin; i < end; ++i) {
            ids.push_back(static_cast<igIndex>(connectivity->GetValue(i)));
        }

        if (asLines) {
            if (ids.size() >= 2) m_Data.GetLines()->AddCellIds(ids.data(), static_cast<int>(ids.size()));
        } else if (asTriangleStrips) {
            if (ids.size() < 3) continue;
            for (size_t i = 0; i + 2 < ids.size(); ++i) {
                igIndex tri[3] = {ids[i], ids[i + 1], ids[i + 2]};
                if (i % 2 == 1) std::swap(tri[0], tri[1]);
                m_Data.GetFaces()->AddCellIds(tri, 3);
            }
        } else if (ids.size() >= 3) {
            m_Data.GetFaces()->AddCellIds(ids.data(), static_cast<int>(ids.size()));
        }
    }
    return true;
}

IGAME_NAMESPACE_END
