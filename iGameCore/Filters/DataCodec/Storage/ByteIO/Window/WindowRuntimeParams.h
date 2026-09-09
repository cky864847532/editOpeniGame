#ifndef DATACODEC_STORAGE_BYTEIO_WINDOW_WINDOWRUNTIMEPARAMS_H
#define DATACODEC_STORAGE_BYTEIO_WINDOW_WINDOWRUNTIMEPARAMS_H

#include <cstddef>

namespace datacodec {

inline constexpr std::size_t kBytesPerMiB = 1024u * 1024u;

// 每次范围读写最多搬运 1 MiB，尾窗口使用剩余字节数
inline constexpr std::size_t kIoWindowBytes = kBytesPerMiB;

} // namespace datacodec

#endif
