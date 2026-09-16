#include <DataCodec/API/Entry/DataCodecDecodeEntry.h>
#include <DataCodec/Filter/Adapter/iGameDecodeAdapter.h>

#include <DataCodec/Filter/Adapter/iGameFramePackageDecodeAssembly.h>
#include <iGameInteractor.h>
#include <iGameRenderWindow.h>
#include <iGameScene.h>

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

namespace {

constexpr const char* kEncodedFile = "./Models/comp.igc";

} // 匿名命名空间

int main(const int argc, char** argv) {
    if (argc > 2) {
        std::cerr << "usage: testDataCodecDecode [input]\n";
        return 2;
    }
    const std::filesystem::path encodedFile = argc >= 2 ? argv[1] : kEncodedFile;
    if (!std::filesystem::is_regular_file(encodedFile)) {
        std::cerr << "encoded DataCodec file is missing: " << encodedFile.string() << '\n';
        return 1;
    }

    if (std::filesystem::file_size(encodedFile) == 0u) {
        std::cerr << "encoded DataCodec file is empty\n";
        return 1;
    }

    // 原生层级在核心解码完成后组装
    iGame::iGameFramePackageDecodeAssembly frameAssembly;

    // 完整配置包含session缓存策略 package入口只提取实际消费的配置
    const auto configuration = ::datacodec::MakeDecodeConfigurationParams({
        .validationProfile = ::datacodec::DataCodecDecodeValidationProfile::Required,
    });

    // DecodePackage是DataCodec的直接解码入口
    auto result = ::datacodec::DecodePackage({
        .input = ::datacodec::EncodedInput::File(encodedFile),
        .cellTypeMapping = std::make_shared<iGame::iGameCellTypeMapping>(),
        .attributeSelection = ::datacodec::AttributeSelectionMode::AllAvailable,
        .configuration = configuration.PackageConfiguration(),
    });
    if (!result.success) {
        std::cerr << "DataCodec decode failed\n";
        for (const auto& message : result.messages) {
            std::cerr << message.text << '\n';
        }
        return 1;
    }

    auto object = frameAssembly.Import(result.output) ? frameAssembly.Output() : iGame::DataObject::Pointer{};
    if (object == nullptr) {
        std::cerr << "DataCodec decode produced no iGame DataObject\n";
        return 1;
    }

    std::cout << "DataCodec decoded file: " << encodedFile.string() << '\n';
    std::cout << "DataCodec package kind: "
              << (result.decodedFramePackage ? "FramePackage" : "LeafPackage") << '\n';
    std::cout << "DataCodec input bytes: " << result.inputBytes << '\n';
    std::cout << "iGame object name: " << object->GetName() << '\n';

    // 将解码后的DataObject加入iGame场景并启动交互窗口
    auto scene = iGame::Scene::New();
    scene->AddModel(object);

    auto window = iGame::RenderWindow::New();
    window->SetSize(1920, 1080);
    window->SetScene(scene);

    auto interactor = iGame::Interactor::New();
    interactor->Initialize(scene);
    interactor->CreateDefaultStyle();
    window->SetInteractor(interactor);
    window->Show();
    return 0;
}
