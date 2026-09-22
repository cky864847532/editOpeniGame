#include "iGameCASReader.h"
#include "iGameExternalProcess.h"
#include <filesystem>
#include <iGameFileIO.h>
#include <iGameScene.h>
#include "Log/iGameLogger.h"

IGAME_NAMESPACE_BEGIN

bool CASReader::Parsing() {
    namespace fs = std::filesystem;

    // ??? .cas ???·??
    std::string casPath = this->GetFilePath();
    fs::path inputPath = FileSystem::PathFromUtf8(casPath);

    // ===== ????????·?? =====
    fs::path tempDir = fs::current_path() / "temp";
    if (!fs::exists(tempDir)) { fs::create_directories(tempDir); }

    // ????¼??? temp ?????
    std::string outputDir = FileSystem::PathToUtf8(tempDir);

    // ????¼?????????????¼
    //std::string outputDir = inputPath.parent_path().string();

    // cas_converter.exe ??·??
    // 尝试多个可能的 exe 路径位置
    std::vector<std::string> exePaths = {
                                        "Resources\\pyNastranLib\\cas_converter.exe",
                                        "Resources\\pyFluentLib\\cas_converter.exe",
                                         "cas_converter.exe"

    };

    std::string exePath;
    bool exeFound = false;
    for (const auto& path: exePaths) {
        // 简单检查文件是否存在
        if (fs::exists(FileSystem::PathFromUtf8(path))) {
            exePath = path;
            exeFound = true;
            break;
        }
    }

    if (!exeFound) {
        IGAME_ERROR("[CASReader] Error: cas_converter.exe not found in any path");
        IGAME_ERROR("[CASReader] Please ensure the exe file exists");
        return false;
    }

    //std::string exePath = "./ThirdParty/Python/pyFluentLib/cas_converter.exe";
    //std::string exePath = "cas_converter.exe";


    // ?????????????

    // Pass UTF-8 arguments directly to the native process API, including spaces and Unicode.
    std::vector<std::string> arguments = {"--input", casPath, "--output", outputDir};
    IGAME_CORE_DEBUG("[CASReader] Running converter: {} --input {} --output {}", exePath, casPath, outputDir);

    int returnCode = 0;
    if (!ExternalProcess::Run(exePath, arguments, returnCode)) {
        IGAME_CORE_ERROR("[CASReader] Failed to start converter: {}", exePath);
        return false;
    }
    if (returnCode != 0) {
        IGAME_CORE_ERROR("CAS to VTK conversion failed. Return code: {}", returnCode);
        return false;
    }


    // ???????????·??
    fs::path outputFileName = inputPath.stem();
    outputFileName += ".vtk";
    fs::path outputFilePath = tempDir / outputFileName;
    std::string outputFile = FileSystem::PathToUtf8(outputFilePath);

    /*std::string outputFile = "";
    fs::path outputFilePath = inputPath.parent_path() / (inputPath.stem().string() + ".vtk");
    outputFile = outputFilePath.string();*/


    // ??????????? VTK ???
    auto obj = iGame::FileIO::ReadFile(outputFile);
    if (!obj) {
        IGAME_CORE_ERROR("Failed to load generated VTK file: {}", outputFile);
        return false;
    }
    IGAME_CORE_DEBUG("DataObjectType: {}", obj->GetDataObjectType());
    auto pc = DynamicCast<PointSet>(obj);
    IGAME_CORE_DEBUG("Points: {}", pc->GetNumberOfPoints());
    // ??????????

    this->SetOutput(obj);
    IGAME_CORE_DEBUG("Successfully converted and loaded: {}", outputFile);

    // ===== ?????????? =====
    try {
        if (fs::exists(outputFilePath)) {
            fs::remove(outputFilePath);
            IGAME_CORE_DEBUG("[Cleanup] Removed temporary file: {}", FileSystem::PathToUtf8(outputFilePath));
        }

        // ??? temp ??????????????
        if (fs::exists(tempDir) && fs::is_empty(tempDir)) {
            fs::remove(tempDir);
            IGAME_CORE_DEBUG("[Cleanup] Removed empty temp directory: {}", FileSystem::PathToUtf8(tempDir));
        }
    } catch (const std::exception& e) {
        IGAME_CORE_WARN("Failed to clean temporary files: {}", e.what());
    }


    return true;
}

IGAME_NAMESPACE_END
