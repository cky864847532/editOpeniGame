#ifndef iGameIGDCReader_h
#define iGameIGDCReader_h

#include "DataCodec/Filter/Adapter/iGameDataCodecDataObjectBridge.h"
#include "DataCodec/Filter/Localization/iGameDataCodecHostMessage.h"
#include "DataCodec/API/Adapter/DecodedFrameTypes.h"
#include "DataCodec/API/Params/CodecParamDefaults.h"
#include "DataCodec/API/Params/DecodedFrameCacheParams.h"
#include "DataCodec/API/Params/EncodedInputCacheParams.h"
#include "DataCodec/API/Output/DataCodecOutputSinks.h"
#include "Attribute/iGameAttributeDataSource.h"
#include "iGameFileReader.h"

#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace datacodec { class MemoryByteRangeReader; }

IGAME_NAMESPACE_BEGIN

class IGDCReader : public FileReader {
public:
    I_OBJECT(IGDCReader);
    static Pointer New();

    bool Execute() override;
    bool Parsing() override;
    bool CreateDataObject() override;

    void SetCodecControlParams(const ::datacodec::DecodeControlParams& params);
    void SetDecodeControls(const ::datacodec::DataCodecDecodeConfigurationParams& definition);
    void SetResourceParams(const ::datacodec::CodecResourceParams& resources);
    // owner 必须持有整个视图，调用期间及后续按需读取期间保持内容不变
    void SetMemoryInput(std::shared_ptr<const void> owner, std::span<const std::uint8_t> bytes);
    void SetDecodeOptions(const ::datacodec::DataCodecDecodeOptions& options);
    void SetRequestedFrameIndex(std::uint32_t frameIndex) { m_requestedFrameIndex = frameIndex; }
    void ClearRequestedFrameIndex() { m_requestedFrameIndex.reset(); }
    void SetSelectedFramePaths(std::vector<std::string> framePaths);
    void SetDecodedFrameCachePolicy(const ::datacodec::DecodedFrameCachePolicy& policy);
    void SetEncodedInputCachePolicy(const ::datacodec::EncodedInputCachePolicy& policy);
    void SetLoadAllAvailableAttributes(bool loadAllAvailableAttributes);
    void SetOutputSinks(::datacodec::DataCodecOutputSinks sinks);
    void SetTelemetrySink(std::shared_ptr<::datacodec::IRunRecordSink> sink);
    void SetLanguage(::datacodec::DataCodecLanguage language);

    [[nodiscard]] AttributeDataSourcePointer GetAttributeDataSource() const;

    const std::vector<::datacodec::TelemetryMessageRecord>& GetMessages() const;
    [[nodiscard]] bool DiagnosticsIncomplete() const noexcept;

protected:
    IGDCReader();
    ~IGDCReader() override;

private:
    using FileReader::SetMemoryBuffer;
    struct State;

    DataObject::Pointer m_DecodedOutput;
    std::optional<std::uint32_t> m_requestedFrameIndex;
    std::unique_ptr<State> m_state;
    std::shared_ptr<::datacodec::MemoryByteRangeReader> m_memoryInput;

    bool DecodeInput();
    void RecordMessage(
        iGameDataCodecHostMessageId messageId,
        std::string technicalDetail = {});
};

IGAME_NAMESPACE_END
#endif
