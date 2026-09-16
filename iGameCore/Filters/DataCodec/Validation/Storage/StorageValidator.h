#ifndef DATACODEC_VALIDATION_STORAGE_STORAGEVALIDATOR_H
#define DATACODEC_VALIDATION_STORAGE_STORAGEVALIDATOR_H

#include "DataCodec/Storage/ByteIO/ByteRange.h"
#include "DataCodec/Validation/Result/ValidationResult.h"

namespace datacodec::validation {

class StorageValidator final {
public:
    [[nodiscard]] static ValidationResult ValidateDecodeInput(
        const IByteRangeReader* reader) {
        if (reader == nullptr) {
            return ValidationResult::Failure(
                CodecErrorCode::MissingInput,
                ValidationDomain::Storage,
                "input.reader",
                "decode package requires an input byte range reader");
        }
        if (reader->ByteSize() < sizeof(std::uint32_t) + sizeof(std::uint16_t)) {
            return ValidationResult::Failure(
                CodecErrorCode::IncompleteInput,
                ValidationDomain::Storage,
                "package.preamble",
                "package header is incomplete");
        }
        return ValidationResult::Success(
            ValidationDomain::Storage,
            "input.reader");
    }
};

} // namespace datacodec::validation

#endif
