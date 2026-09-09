#ifndef DATACODEC_CODEC_ATTRIBUTES_ATTRIBUTEREFERENCEDECODE_H
#define DATACODEC_CODEC_ATTRIBUTES_ATTRIBUTEREFERENCEDECODE_H

#include "DataCodec/Codec/Attributes/AttributeDecode.h"
#include "DataCodec/Storage/ByteStore/ByteStore.h"
#include "DataCodec/Runtime/Cache/CacheResources.h"
#include "DataCodec/Codec/Reference/DecodedReferenceBuilder.h"
#include "DataCodec/Validation/Common/DataCodecValidation.h"
#include "DataCodec/Storage/LeafPackage/LeafPackageFieldDecodeStream.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <array>
namespace datacodec {

class AttributeReferenceDecodeHelper {
public:
    explicit AttributeReferenceDecodeHelper(const CacheResources& parentRuntime)
        : m_cacheResources(parentRuntime) {}

    bool operator()(
        DecodedAttributeReference& reference,
        const AttrStorageParams& targetMeta,
        const std::string_view label,
        std::string* error = nullptr) const {
        std::size_t referenceAttrIndex = 0u;
        if (!decodeimpl::detail::TryFindReferenceAttrIndex(
                reference.reference.storageParams,
                targetMeta,
                referenceAttrIndex)) {
            return validation::AssignError(error, "encoded attribute reference field metadata does not match target");
        }
        if (reference.store != nullptr && reference.store->Complete(referenceAttrIndex)) {
            return true;
        }

        const auto* field = FindLeafPackageField(reference.reference.leafPackage, FieldType::Attribute);
        if (field == nullptr) {
            return validation::AssignError(error, "encoded attribute reference field is missing");
        }

        const auto& cacheResources = m_cacheResources;
        if (reference.store == nullptr) {
            reference.store = std::make_shared<DecodedAttributeCacheSet>();
        }
        if (reference.byteStoreSession == nullptr) {
            reference.byteStoreSession = std::make_shared<bytestore::ByteStoreSession>();
        }
        reference.byteStoreSession->BindRun(cacheResources.Run());
        struct StorageBindingGuard {
            bytestore::ByteStoreSession& session;
            ~StorageBindingGuard() { session.UnbindRun(); }
        } storageBinding{*reference.byteStoreSession};
        std::shared_ptr<bytestore::IByteSource> payload;
        if (!decodefield::PrepareLeafPackageFieldPayload(*field, cacheResources,
                *reference.byteStoreSession, payload, error)) { return false; }
        decodeimpl::detail::AttributePayloadDecodeRuntime decodeRuntime{
            .data = decodeimpl::detail::AttributeDecodeData{
                .storageParams = reference.reference.storageParams,
                .attributeKeyFrameReference = nullptr,
            },
            .cache = decodeimpl::detail::AttributeDecodeCache{
                .cacheResources = cacheResources,
                .byteStoreSession = *reference.byteStoreSession,
                .attributes = *reference.store,
            },
        };
        const std::array<std::size_t, 1u> targetIndices{referenceAttrIndex};
        if (!decodeimpl::detail::DecodeAttributePayloadRangesToCache(
                decodeRuntime,
                *payload,
                targetIndices,
                *this,
                error)) {
            return false;
        }

        if (!reference.store->Complete(referenceAttrIndex)) {
            return validation::AssignError(error, std::string(label) + " requested reference cache is incomplete");
        }
        return true;
    }

private:
    const CacheResources& m_cacheResources;
};

} // namespace datacodec

#endif
