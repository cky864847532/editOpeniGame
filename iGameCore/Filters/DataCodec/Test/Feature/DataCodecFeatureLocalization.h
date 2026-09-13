#ifndef DATACODEC_TEST_FEATURE_DATACODECFEATURELOCALIZATION_H
#define DATACODEC_TEST_FEATURE_DATACODECFEATURELOCALIZATION_H

#include "DataCodec/Localization/DataCodecMessageCatalog.h"
#include "DataCodec/Test/Common/DataCodecTestResult.h"
#include "DataCodec/Runtime/Output/DataCodecOutputRouter.h"
#include "DataCodec/Runtime/Record/RunRecordEmitter.h"

#include <string>
#include <vector>

namespace datacodec::test {

[[nodiscard]] inline TestResult RunDataCodecFeatureLocalization() {
    TestResult result;

    const std::vector<DataCodecMessageArgument> attributeArguments{
        {"name", "Pressure"},
    };
    Require(
        result,
        FormatDataCodecMessage(
            DataCodecLanguage::SimplifiedChinese,
            DataCodecMessageId::EncodeAttributeNamed,
            attributeArguments) == "属性数据压缩：Pressure",
        "localization.zh.namedArgument",
        "Chinese message formatting should replace named arguments");
    Require(
        result,
        FormatDataCodecMessage(
            DataCodecLanguage::English,
            DataCodecMessageId::EncodeAttributeNamed,
            attributeArguments) == "Compressing attribute data: Pressure",
        "localization.en.namedArgument",
        "English message formatting should replace named arguments");

    const std::vector<DataCodecMessageArgument> frameArguments{
        {"count", "8"},
        {"index", "3"},
    };
    Require(
        result,
        FormatDataCodecMessage(
            DataCodecLanguage::English,
            DataCodecMessageId::FrameCounter,
            frameArguments) == "Frame 3/8",
        "localization.namedArgumentOrder",
        "message formatting should not depend on argument order");

    const auto localized = LocalizeDataCodecMessage(
        DataCodecLanguage::SimplifiedChinese,
        DataCodecMessageId::DecodeCompleted,
        {},
        "decoder returned a malformed payload");
    Require(
        result,
        localized.language == DataCodecLanguage::SimplifiedChinese &&
            localized.id == DataCodecMessageId::DecodeCompleted &&
            localized.text == "解压完成" &&
            localized.technicalDetail == "decoder returned a malformed payload",
        "localization.structuredMessage",
        "localized messages should preserve language, id, text, and technical detail");
    Require(
        result,
        std::string(DataCodecMessageIdName(DataCodecMessageId::DecodeCompleted)) ==
            "DecodeCompleted",
        "localization.stableMessageId",
        "message ids should have stable language-independent names");

    struct StatusCapture final : IDataCodecUiSink {
        std::vector<DataCodecStatusRecord> statuses;
        void SubmitUiStatus(const DataCodecStatusRecord& status) override { statuses.push_back(status); }
    };
    // 沿实际失败导出和输出路由验证请求语言，同时覆盖普通错误与警告
    for (const auto language : {DataCodecLanguage::English, DataCodecLanguage::SimplifiedChinese}) {
        for (const auto kind : {TelemetryRunKind::Encode, TelemetryRunKind::Decode}) {
            auto capture = std::make_shared<StatusCapture>();
            DataCodecOutputRouter router({.ui = capture});
            RunRecordEmitter emitter;
            emitter.Reset({.runKind = kind, .language = language}, &router);
            const auto failure = MakeCodecFailureRecord(CodecErrorCode::PipelineFailure,
                "test-failure", "TestStage", "allocation rejected");
            emitter.TryEndFailedRun(failure);
            emitter.AddError("TestStage", "ordinary error");
            emitter.AddWarning("TestStage", "ordinary warning");
            const auto retained = emitter.TakeMessages();
            const auto failedId = kind == TelemetryRunKind::Encode
                ? DataCodecMessageId::EncodeFailed : DataCodecMessageId::DecodeFailed;
            const auto warningId = kind == TelemetryRunKind::Encode
                ? DataCodecMessageId::EncodeWarning : DataCodecMessageId::DecodeWarning;
            Require(result, capture->statuses.size() == 3u && retained.size() == 3u,
                "localization.failure.outputs", "failure, error and warning should all reach the status sink");
            if (capture->statuses.size() == 3u && retained.size() == 3u) {
                for (std::size_t i = 0u; i < 3u; ++i) {
                    const auto& status = capture->statuses[i];
                    const auto id = i == 2u ? warningId : failedId;
                    Require(result, status.language == language && retained[i].language == language &&
                        status.messageId == id && status.text == FormatDataCodecMessage(language, id) &&
                        status.technicalDetail == retained[i].text,
                        "localization.failure.inherited-language",
                        "routed failure titles and retained raw messages must inherit request language and preserve technical detail");
                }
            }
        }
    }

    return result;
}

} // 命名空间 datacodec::test

#endif
