#ifndef DATACODEC_API_PARAMS_ENCODEDINPUTCACHEPARAMS_H
#define DATACODEC_API_PARAMS_ENCODEDINPUTCACHEPARAMS_H
namespace datacodec {
// 完整输入预读是可选业务优化，容量与最多一份留存由根管理
struct EncodedInputCachePolicy {
    bool enabled{false};
};
}
#endif
