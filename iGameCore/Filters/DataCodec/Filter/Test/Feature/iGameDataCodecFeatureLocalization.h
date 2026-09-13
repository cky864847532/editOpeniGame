#ifndef iGameDataCodecFeatureLocalization_h
#define iGameDataCodecFeatureLocalization_h

#include "DataCodec/Filter/Localization/iGameDataCodecHostMessage.h"
#include "DataCodec/Test/Common/DataCodecTestResult.h"

#include <iostream>

namespace iGame::datacodec_test {

[[nodiscard]] inline int RunDataCodecFeatureHostLocalization() {
    ::datacodec::test::TestResult result;

    ::datacodec::test::Require(
        result,
        iGameDataCodecHostMessage(
            ::datacodec::DataCodecLanguage::SimplifiedChinese,
            iGameDataCodecHostMessageId::OutputPathSet,
            {{"path", "D:/data/output.igc"}}) ==
            "输出路径已设置为：D:/data/output.igc",
        "hostLocalization.zh.namedArgument",
        "Chinese host messages should replace named arguments");
    ::datacodec::test::Require(
        result,
        iGameDataCodecHostMessage(
            ::datacodec::DataCodecLanguage::English,
            iGameDataCodecHostMessageId::MissingRegionFeatures,
            {{"count", "3"}}) ==
            "Cannot start compression: 3 fields have custom regions without computed features",
        "hostLocalization.en.namedArgument",
        "English host messages should replace named arguments");
    ::datacodec::test::Require(
        result,
        iGameDataCodecHostMessage(
            ::datacodec::DataCodecLanguage::SimplifiedChinese,
            iGameDataCodecHostMessageId::DecodeFailed) == "iGame DataCodec 解码失败" &&
            iGameDataCodecHostMessage(
                ::datacodec::DataCodecLanguage::English,
                iGameDataCodecHostMessageId::DecodeFailed) ==
                "iGame DataCodec decoding failed",
        "hostLocalization.languageSelection",
        "host message language selection should not depend on Qt translation state");
    ::datacodec::test::Require(
        result,
        iGameDataCodecHostMessage(
            ::datacodec::DataCodecLanguage::SimplifiedChinese,
            iGameDataCodecHostMessageId::CompressionRatioCalculationNote) ==
                "压缩率采用小数表示，计算公式为：压缩结果文件大小 ÷ 源文件大小" &&
            iGameDataCodecHostMessage(
                ::datacodec::DataCodecLanguage::English,
                iGameDataCodecHostMessageId::CompressionRatioCalculationNote) ==
                "The compression ratio is expressed as a decimal value and calculated by dividing the compressed output file size by the source file size.",
        "hostLocalization.compressionRatioCalculationNote",
        "compression ratio calculation note should provide equivalent Chinese and English wording");

    const auto rejected = iGameDataCodecHostStatus(::datacodec::DataCodecLanguage::English,
        iGameDataCodecHostMessageId::StorageLimitInsufficient,
        {{"mib", "2"}, {"bytes", "1048577"}}, ::datacodec::DataCodecStatusSeverity::Error);
    ::datacodec::test::Require(result,
        rejected.text.find("2 MiB (1048577 bytes)") != std::string::npos &&
        rejected.severity == ::datacodec::DataCodecStatusSeverity::Error,
        "hostLocalization.capacity-rejection", "capacity messages must substitute byte counts and preserve severity");
    const auto failed = iGameDataCodecHostStatus(::datacodec::DataCodecLanguage::English,
        iGameDataCodecHostMessageId::StorageCheckFailed, {},
        ::datacodec::DataCodecStatusSeverity::Warning, "I/O error at offset 42");
    ::datacodec::test::Require(result, failed.text == "The capacity check did not complete" &&
        failed.technicalDetail == "I/O error at offset 42" &&
        failed.language == ::datacodec::DataCodecLanguage::English,
        "hostLocalization.technical-detail", "host status must separate localized business text from diagnostic details");
    for (unsigned id = static_cast<unsigned>(iGameDataCodecHostMessageId::StorageCheckUpdating);
         id <= static_cast<unsigned>(iGameDataCodecHostMessageId::WaitBeforeFileSelection); ++id) {
        const auto message = static_cast<iGameDataCodecHostMessageId>(id);
        const auto en = iGameDataCodecHostMessage(::datacodec::DataCodecLanguage::English, message);
        const auto zh = iGameDataCodecHostMessage(::datacodec::DataCodecLanguage::SimplifiedChinese, message);
        ::datacodec::test::Require(result, !en.empty() && !zh.empty() && en != zh,
            "hostLocalization.new-message-coverage", "each new host message must have Chinese and English templates");
    }

    for (const auto& failure : result.failures) {
        std::cerr << failure.check << ": " << failure.message << '\n';
    }
    return result.passed ? 0 : 1;
}

} // 命名空间 iGame::datacodec_test

#endif
