#ifndef DATACODEC_API_PARAMS_DECODEDFRAMECACHEPARAMS_H
#define DATACODEC_API_PARAMS_DECODEDFRAMECACHEPARAMS_H
namespace datacodec {
// 业务开关只决定是否请求留存，数量与压力政策由根管理
struct DecodedFrameCachePolicy {
    bool enabled{true};
    bool prefetchEnabled{true};
};
}
#endif
