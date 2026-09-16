#include "DataCodec/API/Entry/DataCodecFrameSequenceEncode.h"
#include "DataCodec/Workflow/FrameSequence/FrameSequenceEncodeExecutor.h"

namespace datacodec {

FrameSequenceEncodeResult EncodeFrameSequence(const FrameSequenceEncodeRequest& request) noexcept {
    return FrameSequenceEncodeExecutor::Execute(request);
}

} // 命名空间 datacodec
