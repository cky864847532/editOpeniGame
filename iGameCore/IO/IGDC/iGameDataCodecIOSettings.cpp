#include "iGameDataCodecIOSettings.h"


#include <mutex>

IGAME_NAMESPACE_BEGIN

namespace {

std::mutex& DefaultDecodeOptionsMutex() {
    static std::mutex mutex;
    return mutex;
}

::datacodec::DataCodecDecodeOptions& DefaultDecodeOptions() {
    static ::datacodec::DataCodecDecodeOptions options;
    return options;
}

bool& DefaultLoadAllAvailableAttributes() {
    static bool loadAllAvailableAttributes = true;
    return loadAllAvailableAttributes;
}

::datacodec::CodecResourceParams& DefaultDecodeResources() {
    static ::datacodec::CodecResourceParams resources;
    return resources;
}

} // namespace

::datacodec::DataCodecDecodeOptions DataCodecIOSettings::GetDefaultDecodeOptions() {
    std::scoped_lock lock(DefaultDecodeOptionsMutex());
    return DefaultDecodeOptions();
}

void DataCodecIOSettings::SetDefaultDecodeOptions(const ::datacodec::DataCodecDecodeOptions& options) {
    std::scoped_lock lock(DefaultDecodeOptionsMutex());
    DefaultDecodeOptions() = options;
}

bool DataCodecIOSettings::GetDefaultLoadAllAvailableAttributes() {
    std::scoped_lock lock(DefaultDecodeOptionsMutex());
    return DefaultLoadAllAvailableAttributes();
}

::datacodec::CodecResourceParams DataCodecIOSettings::GetDefaultDecodeResources() {
    std::scoped_lock lock(DefaultDecodeOptionsMutex());
    return DefaultDecodeResources();
}

void DataCodecIOSettings::SetDefaultDecodeResources(const ::datacodec::CodecResourceParams& resources) {
    std::scoped_lock lock(DefaultDecodeOptionsMutex());
    DefaultDecodeResources() = resources;
}

void DataCodecIOSettings::SetDefaultLoadAllAvailableAttributes(
        const bool loadAllAvailableAttributes) {
    std::scoped_lock lock(DefaultDecodeOptionsMutex());
    DefaultLoadAllAvailableAttributes() = loadAllAvailableAttributes;
}

IGAME_NAMESPACE_END
