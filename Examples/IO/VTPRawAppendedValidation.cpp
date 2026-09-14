#include <VTK XML/iGameVTPReader.h>
#include <iGameSurfaceMesh.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
template<typename T>
void Append(std::string& bytes, const std::vector<T>& values) {
    bytes.append(reinterpret_cast<const char*>(values.data()), values.size() * sizeof(T));
}

template<typename Header, typename Coordinate>
std::string Fixture(const std::string& corruption = {}, bool quad = false) {
    std::vector<Coordinate> coordinates = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
    std::vector<int32_t> connectivity = quad ? std::vector<int32_t>{0, 1, 2, 3}
                                           : std::vector<int32_t>{0, 1, 2, 0, 2, 3};
    std::vector<int64_t> offsets = quad ? std::vector<int64_t>{4} : std::vector<int64_t>{3, 6};
    std::vector<float> scalar = {-4.5f, 1.0f, 0.5f, -0.25f};
    if (corruption == "index") connectivity.back() = 4;
    if (corruption == "offset") offsets.back() = 5;
    if (corruption == "coordinate") coordinates[1] = std::numeric_limits<Coordinate>::infinity();
    if (corruption == "scalar") scalar[1] = std::numeric_limits<float>::quiet_NaN();
    std::string payload;
    auto block = [&](const auto& values) {
        const size_t offset = payload.size();
        const Header length = static_cast<Header>(values.size() * sizeof(values[0]));
        Append(payload, std::vector<Header>{length});
        Append(payload, values);
        return offset;
    };
    const size_t xyzOffset = block(coordinates);
    const size_t connectivityOffset = block(connectivity);
    const size_t offsetsOffset = block(offsets);
    const size_t scalarOffset = block(scalar);
    if (corruption == "length") {
        const Header badLength = std::numeric_limits<Header>::max();
        std::memcpy(payload.data(), &badLength, sizeof(badLength));
    }
    const std::string declaredXyzOffset = corruption == "seek" ? "4294967296" : std::to_string(xyzOffset);
    std::string xml = "<?xml version=\"1.0\"?>\n<VTKFile type=\"PolyData\" version=\"1.0\" byte_order=\"LittleEndian\" header_type=\"";
    xml += sizeof(Header) == 8 ? "UInt64" : "UInt32";
    xml += "\"><PolyData><Piece NumberOfPoints=\"4\" NumberOfVerts=\"0\" NumberOfLines=\"0\" NumberOfStrips=\"0\" NumberOfPolys=\"";
    xml += quad ? "1" : "2";
    xml += "\"><Points><DataArray type=\"";
    xml += sizeof(Coordinate) == 8 ? "Float64" : "Float32";
    xml += "\" NumberOfComponents=\"3\" format=\"appended\" offset=\"" + declaredXyzOffset + "\"/></Points>";
    xml += "<PointData Scalars=\"PressureCoefficient\"><DataArray type=\"Float32\" Name=\"PressureCoefficient\" format=\"appended\" offset=\"" +
           std::to_string(scalarOffset) + "\"/></PointData><CellData/><Polys>";
    xml += "<DataArray type=\"Int32\" Name=\"connectivity\" format=\"appended\" offset=\"" + std::to_string(connectivityOffset) + "\"/>";
    xml += "<DataArray type=\"Int64\" Name=\"offsets\" format=\"appended\" offset=\"" + std::to_string(offsetsOffset) + "\"/>";
    xml += "</Polys></Piece></PolyData><AppendedData encoding=\"raw\">\n_";
    xml += payload;
    if (corruption != "truncated") xml += "\n</AppendedData></VTKFile>\n";
    return xml;
}

void Check(const std::filesystem::path& path, const std::string& bytes, bool expected, bool quad = false) {
    {
        std::ofstream output(path, std::ios::binary);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!output) throw std::runtime_error("cannot write test fixture");
    }
    auto reader = iGame::iGameVTPReader::New();
    reader->SetFilePath(path.string());
    const bool success = reader->Execute();
    if (success != expected) throw std::runtime_error("unexpected reader result for " + path.string());
    if (!success) {
        if (reader->GetOutput()) throw std::runtime_error("failed read published an output");
    } else {
        auto mesh = iGame::DynamicCast<iGame::SurfaceMesh>(reader->GetOutput());
        if (!mesh || mesh->GetNumberOfPoints() != 4 || mesh->GetFaces()->GetNumberOfCells() != (quad ? 1 : 2)) {
            throw std::runtime_error("wrong output mesh counts");
        }
        igIndex ids[4] = {};
        const int count = mesh->GetFaces()->GetCellIds(quad ? 0 : 1, ids);
        if (count != (quad ? 4 : 3) || ids[count - 1] != 3) throw std::runtime_error("wrong connectivity");
        const auto& field = mesh->GetAttributeSet()->GetAttribute(0);
        if (!field.pointer || field.attachmentType != IG_POINT || field.pointer->GetName() != "PressureCoefficient" ||
            field.pointer->GetNumberOfValues() != 4 || field.pointer->GetValue(0) != -4.5 || field.pointer->GetValue(3) != -0.25) {
            throw std::runtime_error("point scalar was not preserved");
        }
    }
    std::filesystem::remove(path);
}
} // namespace

int main() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path() / ("igame-vtp-raw-validation-" + std::to_string(stamp));
    try {
        std::filesystem::create_directory(directory);
        Check(directory / "double-u64.vtp", Fixture<uint64_t, double>(), true);
        Check(directory / "float-u32.vtp", Fixture<uint32_t, float>(), true);
        for (const std::string bad : {"index", "offset", "coordinate", "scalar", "length", "seek", "truncated"}) {
            Check(directory / (bad + ".vtp"), Fixture<uint64_t, double>(bad), false);
        }
        Check(directory / "quad-fallback.vtp", Fixture<uint64_t, double>({}, true), true, true);
        const std::string ascii =
            "<VTKFile type=\"PolyData\" byte_order=\"LittleEndian\"><PolyData>"
            "<Piece NumberOfPoints=\"4\" NumberOfPolys=\"2\"><Points>"
            "<DataArray type=\"Float32\" NumberOfComponents=\"3\">0 0 0 1 0 0 1 1 0 0 1 0</DataArray></Points>"
            "<PointData><DataArray Name=\"PressureCoefficient\" type=\"Float32\">-4.5 1 0.5 -0.25</DataArray></PointData>"
            "<Polys><DataArray Name=\"connectivity\" type=\"Int32\">0 1 2 0 2 3</DataArray>"
            "<DataArray Name=\"offsets\" type=\"Int64\">3 6</DataArray></Polys></Piece></PolyData></VTKFile>";
        Check(directory / "ascii-fallback.vtp", ascii, true);
        std::filesystem::remove(directory);
        std::cout << "VTP raw appended validation: 11 cases passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << " (fixtures: " << directory << ")\n";
        return 1;
    }
}
