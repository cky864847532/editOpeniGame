#ifndef DATACODEC_STORAGE_BYTESTORE_DECODEDBUFFERACCESS_H
#define DATACODEC_STORAGE_BYTESTORE_DECODEDBUFFERACCESS_H

#include "DataCodec/API/Output/DecodedBuffer.h"
#include "DataCodec/Storage/ByteStore/ByteStore.h"

namespace datacodec {

class DecodedBufferAccess final {
public:
    static bool Export(bytestore::IByteSource& source, bytestore::ByteStoreSession& session,
                       DecodedBuffer& output, std::string* error = nullptr) {
        if (session.Stopped()) { return validation::AssignError(error, "decoded result transfer cancelled"); }
        if (!source.CanRead()) {
            return validation::AssignError(error, "decoded result requires sealed storage");
        }
        std::shared_ptr<bytestore::IByteStore> materialized;
        auto* memory = dynamic_cast<bytestore::MemoryStore*>(&source);
        if (!memory) {
            // 范围存储按最终连续数组容量准入，直接读入交付分配
            materialized = session.CreateSizedStore(bytestore::ByteStorePurpose::Contiguous,
                source.ByteSizeHint(), MemoryDemandKind::RequiredContinuation, "decoded_result", error);
            memory = dynamic_cast<bytestore::MemoryStore*>(materialized.get());
            if (!memory) { return false; }
            auto target = memory->WritableBytes();
            for (std::size_t offset = 0u; offset < target.size();) {
                if (session.Stopped()) { return validation::AssignError(error, "decoded result transfer cancelled"); }
                const auto length = std::min<std::size_t>(target.size() - offset, kIoWindowBytes);
                if (!source.Read(offset, target.subspan(offset, length), error)) { return false; }
                offset += length;
            }
            if (!memory->Seal(error)) { return false; }
        }
        if (session.Stopped()) { return validation::AssignError(error, "decoded result transfer cancelled"); }
        DecodedBuffer exported;
        exported.m_size = static_cast<std::size_t>(memory->ByteSizeHint());
        exported.m_capacity = static_cast<std::size_t>(memory->ResidentSizeHint());
        exported.m_bytes = memory->ShareOwnedBytes();
        if (exported.m_size != 0u && !exported.m_bytes) {
            return validation::AssignError(error, "decoded storage ownership transfer failed");
        }
        output = std::move(exported);
        return true;
    }
};

} // 命名空间 datacodec
#endif
