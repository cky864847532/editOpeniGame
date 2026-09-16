#ifndef DATACODEC_API_ENTRY_PACKAGEDECODESESSION_H
#define DATACODEC_API_ENTRY_PACKAGEDECODESESSION_H

#include "DataCodec/API/Entry/DataCodecDecodeEntry.h"
#include "DataCodec/API/Adapter/DecodedFrameTypes.h"
#include "DataCodec/API/Adapter/EncodedInputTypes.h"
#include "DataCodec/API/Params/EncodedInputCacheParams.h"


namespace datacodec {

struct PackageDecodeSessionOpenRequest {
    DecodePackageRequest decode;
    DecodeSourceIdentity sourceIdentity;
    EncodedInputCachePolicy encodedInputCachePolicy;
};

struct PackageDecodeSessionAttributeRequest {
    std::vector<AttributeTarget> targets;
    AttributeDecodeRequestMode mode{AttributeDecodeRequestMode::DecodeAndCommit};
    std::shared_ptr<IRunRecordSink> runRecordSink;
    std::stop_token stopToken;
};

// 持有打开包及按需属性请求的资源、缓存和准备状态
// 所有公开操作由调用方串行安排，stop_token 可从其他线程请求取消
// 结果只拥有数据，准备中的属性由会话保留至提交、失败或 Reset
class PackageDecodeSession final {
public:
    PackageDecodeSession();
    ~PackageDecodeSession();
    PackageDecodeSession(const PackageDecodeSession&) = delete;
    PackageDecodeSession& operator=(const PackageDecodeSession&) = delete;

    [[nodiscard]] DecodePackageResult Open(const PackageDecodeSessionOpenRequest& request);
    [[nodiscard]] DecodePackageResult RequestAttributes(const PackageDecodeSessionAttributeRequest& request);
    [[nodiscard]] std::vector<DecodeAttributeDescriptor> AvailableAttributes() const;
    [[nodiscard]] EncodedInputCacheStats InputCacheStatistics() const;
    [[nodiscard]] bool IsOpen() const noexcept;
    void Reset() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // 命名空间 datacodec

#endif
