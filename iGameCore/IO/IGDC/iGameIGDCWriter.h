#ifndef iGameIGDCWriter_h
#define iGameIGDCWriter_h

#include "DataCodec/API/Entry/DataCodecEncodeEntry.h"
#include "DataCodec/API/Entry/EncodeStorageAnalysis.h"
#include "DataCodec/Filter/Localization/iGameDataCodecHostMessage.h"
#include "iGameFileWriter.h"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

IGAME_NAMESPACE_BEGIN

class IGDCWriter : public FileWriter {
public:
    I_OBJECT(IGDCWriter);
    static Pointer New() { return new IGDCWriter; }

    bool Execute() override;
    bool GenerateBuffers() override;

    // 复用写入时的属性选择和参数，未载入的时序帧只记录未知项
    [[nodiscard]] ::datacodec::EncodeStorageAnalysisResult AnalyzeStorage(const DataObject::Pointer& data) const;
    [[nodiscard]] std::optional<::datacodec::CodecFailureRecord> CheckStorageBeforeEncode(
        const DataObject::Pointer& data) const;

    void SetEncodeControls(const ::datacodec::DataCodecEncodeConfigurationParams& definition) {
        m_hasCodecParams = true;
        m_CodecParams = definition.controlParams;
        m_pipelineControl = definition.pipelineControl;
        m_configurationSource = definition.source;
        m_language = definition.language;
    }

    void SetAttributeTargets(std::vector<::datacodec::AttributeTarget> targets) {
        m_hasAttributeTargets = true;
        m_attributeTargets = std::move(targets);
    }

    void ClearAttributeTargets() {
        m_hasAttributeTargets = false;
        m_attributeTargets.clear();
    }

    void SetOutputSinks(::datacodec::DataCodecOutputSinks sinks) noexcept {
        m_outputSinks = std::move(sinks);
    }

    void SetTelemetrySink(std::shared_ptr<::datacodec::IRunRecordSink> sink) noexcept {
        m_telemetrySink = std::move(sink);
    }

    [[nodiscard]] const std::vector<std::string>& GetWrittenFilePaths() const noexcept {
        return m_writtenFilePaths;
    }

    [[nodiscard]] bool DiagnosticsIncomplete() const noexcept { return m_diagnosticsIncomplete; }

    void SetResourceParams(const ::datacodec::CodecResourceParams& resources) {
        m_resources = resources;
    }

    void SetEncodeOptions(const ::datacodec::DataCodecEncodeOptions& options) {
        SetEncodeControls(::datacodec::MakeEncodeConfigurationParams(options));
    }

protected:
    IGDCWriter() = default;
    ~IGDCWriter() override = default;

private:
    bool m_hasCodecParams = false;
    bool m_hasAttributeTargets{false};
    bool m_diagnosticsIncomplete{false};
    ::datacodec::CodecControlParams m_CodecParams;
    ::datacodec::EncodePipelineControlParams m_pipelineControl;
    ::datacodec::CodecResourceParams m_resources;
    ::datacodec::DataCodecEncodeConfigurationSource m_configurationSource;
    ::datacodec::DataCodecLanguage m_language{
        ::datacodec::DataCodecLanguage::SimplifiedChinese};
    std::vector<::datacodec::AttributeTarget> m_attributeTargets;
    ::datacodec::DataCodecOutputSinks m_outputSinks;
    std::shared_ptr<::datacodec::IRunRecordSink> m_telemetrySink;
    std::vector<std::string> m_writtenFilePaths;

    ::datacodec::EncodePackageKind ResolveEncodePackageKind(const DataObject::Pointer& rootObject) const;
    bool EncodeToFile(::datacodec::EncodePackageKind packageKind);
    bool EncodeFrameSequence(const std::filesystem::path& outputHint);
    void RecordMessage(
        iGameDataCodecHostMessageId messageId,
        std::string technicalDetail = {});
};

IGAME_NAMESPACE_END
#endif
