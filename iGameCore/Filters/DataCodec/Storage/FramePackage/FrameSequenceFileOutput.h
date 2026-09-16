#ifndef DATACODEC_STORAGE_FRAMEPACKAGE_FRAMESEQUENCEFILEOUTPUT_H
#define DATACODEC_STORAGE_FRAMEPACKAGE_FRAMESEQUENCEFILEOUTPUT_H

#include "DataCodec/API/Entry/DataCodecFrameSequenceEncode.h"
#include "DataCodec/Storage/ByteIO/FileByteRangeIO.h"
#include <memory>

namespace datacodec {

// 序列在核心内部暂存，全部编码完成后提交，失败恢复原有文件
class FrameSequenceFileOutput final {
public:
    explicit FrameSequenceFileOutput(FrameSequenceFileTarget target);
    ~FrameSequenceFileOutput();
    std::unique_ptr<FileByteRangeOutput> OpenFrame(std::uint32_t index,
        std::filesystem::path& finalPath, std::string* error);
    bool Commit(std::string* error);
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // 命名空间 datacodec
#endif
