#include "Codec/iGameWasmDecodedModelRegistry.h"
#include "Codec/iGameWasmDataCodecBridge.h"

#include "DataCodec/Platform/Wasm/WasmBrowserFileByteRangeReader.h"

#include <cstdio>
#include <utility>

IGAME_NAMESPACE_BEGIN

iGameWasmDecodedModelRegistry::~iGameWasmDecodedModelRegistry() {
    Clear();
}

bool iGameWasmDecodedModelRegistry::Contains(const std::uint32_t modelId) const {
    return m_entries.contains(modelId);
}

int iGameWasmDecodedModelRegistry::FindBySource(
    const ::datacodec::DecodeSourceIdentity& sourceIdentity, const bool requireRawData) const {
    if (!sourceIdentity.IsStable()) { return 0; }
    for (const auto& [modelId, entry] : m_entries) {
        if (entry.sourceIdentity == sourceIdentity && (!requireRawData || (entry.codec && entry.codec->IsOpen()))) {
            return static_cast<int>(modelId);
        }
    }
    return 0;
}

iGameWasmDecodedModelEntry* iGameWasmDecodedModelRegistry::Find(
    const std::uint32_t modelId) {
    const auto iterator = m_entries.find(modelId);
    return iterator != m_entries.end() ? &iterator->second : nullptr;
}

const iGameWasmDecodedModelEntry* iGameWasmDecodedModelRegistry::Find(
    const std::uint32_t modelId) const {
    const auto iterator = m_entries.find(modelId);
    return iterator != m_entries.end() ? &iterator->second : nullptr;
}

void iGameWasmDecodedModelRegistry::Store(
    const std::uint32_t modelId,
    iGameWasmDecodedModelEntry entry) {
    Erase(modelId);
    m_entries.emplace(modelId, std::move(entry));
}

DataObject::Pointer iGameWasmDecodedModelRegistry::RestoreRawData(
    const std::uint32_t modelId, const ::datacodec::CodecResourceParams& resources, std::string* error,
    std::optional<::datacodec::CodecFailureRecord>* failure) {
    if (error) { error->clear(); }
    if (failure) { failure->reset(); }
    auto* entry = Find(modelId);
    if (!entry) {
        if (error) { *error = "model is unavailable"; }
        return nullptr;
    }
    if (entry->codec && entry->codec->IsOpen()) { return entry->codec->GetOutput(); }
    if (!entry->deferredInput) {
        if (error) { *error = "surface model has no retained original input"; }
        return nullptr;
    }
    try {
        const auto revision = m_revision;
        const auto input = entry->deferredInput;
        const auto identity = entry->sourceIdentity;
        auto decoded = DecodeiGameWasmDataCodec({.input = input,
            .sourceIdentity = identity, .resources = resources});
        if (!decoded.success) {
            if (error) { *error = decoded.error; }
            if (failure) { *failure = decoded.decodeResult.failure; }
            return nullptr;
        }
        // 浏览器范围读取可能挂起主线程，恢复后重新验证模型仍然存在
        entry = Find(modelId);
        if (m_revision != revision || !entry) {
            if (error) { *error = "model registry changed while restoring original data"; }
            if (failure) { *failure = ::datacodec::MakeCodecFailureRecord(
                ::datacodec::CodecErrorCode::DecodeFailure, "restore-invalidated",
                "iGameWasmDecodedModelRegistry", "model registry changed while restoring original data", true); }
            return nullptr;
        }
        entry->codec = std::move(decoded.session);
        entry->deferredInput = {};
        return decoded.output;
    } catch (const std::exception& exception) {
        if (error) { *error = exception.what(); }
    } catch (...) {
        if (error) { *error = "original data restoration failed"; }
    }
    return nullptr;
}

void iGameWasmDecodedModelRegistry::Erase(const std::uint32_t modelId) {
    ++m_revision;
    const auto iterator = m_entries.find(modelId);
    if (iterator == m_entries.end()) { return; }
    ReleaseInput(iterator->second);
    m_entries.erase(iterator);
}

void iGameWasmDecodedModelRegistry::Clear() {
    ++m_revision;
    for (auto& [modelId, entry] : m_entries) {
        (void)modelId;
        ReleaseInput(entry);
    }
    m_entries.clear();
}

std::vector<std::uint32_t> iGameWasmDecodedModelRegistry::ModelIds() const {
    std::vector<std::uint32_t> modelIds;
    modelIds.reserve(m_entries.size());
    for (const auto& [modelId, entry] : m_entries) {
        (void)entry;
        modelIds.push_back(modelId);
    }
    return modelIds;
}

void iGameWasmDecodedModelRegistry::ReleaseInput(iGameWasmDecodedModelEntry& entry) {
    if (entry.codec) { entry.codec->Reset(); entry.codec.reset(); }
    entry.deferredInput = {};
    if (!entry.ownedInputPath.empty()) {
        std::remove(entry.ownedInputPath.c_str());
        entry.ownedInputPath.clear();
    }
    if (entry.browserFileId != 0u) {
        ::datacodec::wasm::ReleaseWasmBrowserFile(entry.browserFileId);
        entry.browserFileId = 0u;
    }
}

IGAME_NAMESPACE_END
