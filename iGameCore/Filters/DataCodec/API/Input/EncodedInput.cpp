#include "DataCodec/API/Input/EncodedInput.h"
#include "DataCodec/Storage/ByteIO/EncodedInputAccess.h"
#include "DataCodec/Storage/ByteIO/FileByteRangeIO.h"
#include "DataCodec/Platform/Wasm/WasmBrowserFileByteRangeReader.h"
#include <variant>
#include <stdexcept>

namespace datacodec {

struct EncodedInput::Impl {
    struct Memory {
        std::shared_ptr<const void> owner;
        std::span<const std::uint8_t> bytes;
        std::optional<std::uint64_t> capacity;
    };
    struct Browser {
        std::uint32_t id;
        std::uint64_t size;
        std::shared_ptr<const void> owner;
    };
    using Source = std::variant<std::filesystem::path, Memory, Browser, std::shared_ptr<IByteRangeReader>>;
    Source source;
    explicit Impl(Source value) : source(std::move(value)) {}
};

EncodedInput EncodedInput::File(std::filesystem::path path) {
    return EncodedInput(std::make_shared<Impl>(std::move(path)));
}
InputMemoryObservation EncodedInput::ObserveMemory() const {
    if (!m_impl) { return {}; }
    if (const auto* memory = std::get_if<Impl::Memory>(&m_impl->source)) {
        const InputMemoryView view{memory->bytes.data(), 0u, memory->bytes.size(), memory->capacity};
        return ObserveInputMemory(std::span<const InputMemoryView>(&view, 1u));
    }
    return {};
}
EncodedInput EncodedInput::Memory(std::shared_ptr<const void> owner,
    std::span<const std::uint8_t> bytes, std::optional<std::uint64_t> capacity) {
    if ((!owner && !bytes.empty()) || (capacity && *capacity < bytes.size())) {
        throw std::invalid_argument("shared encoded input requires an owner and a capacity covering its byte range");
    }
    return EncodedInput(std::make_shared<Impl>(Impl::Memory{std::move(owner), bytes, capacity}));
}
EncodedInput EncodedInput::Memory(std::shared_ptr<const std::vector<std::uint8_t>> bytes) {
    if (!bytes) { return {}; }
    const auto range = std::span<const std::uint8_t>(*bytes);
    const auto capacity = bytes->capacity();
    return Memory(std::move(bytes), range, capacity);
}
EncodedInput EncodedInput::Memory(std::shared_ptr<const EncodedBuffer> bytes) {
    if (!bytes) { return {}; }
    const auto range = bytes->span();
    const auto capacity = bytes->capacity();
    return Memory(std::move(bytes), range, capacity);
}
EncodedInput EncodedInput::Memory(EncodedBuffer bytes) {
    return Memory(std::make_shared<const EncodedBuffer>(std::move(bytes)));
}
EncodedInput EncodedInput::BrowserFile(std::uint32_t id, std::uint64_t size, std::shared_ptr<const void> owner) {
    struct BrowserOwner {
        std::uint32_t id{0u};
        std::shared_ptr<const void> owner;
        ~BrowserOwner() { if (id) { wasm::ReleaseWasmBrowserFile(id); } }
    };
    auto retained = std::make_shared<BrowserOwner>();
    if (size == 0u || !wasm::RetainWasmBrowserFile(id)) {
        throw std::invalid_argument("browser file input requires a registered nonempty File");
    }
    retained->id = id;
    retained->owner = std::move(owner);
    return EncodedInput(std::make_shared<Impl>(Impl::Browser{id, size, std::move(retained)}));
}
EncodedInput EncodedInputAccess::Retain(std::shared_ptr<IByteRangeReader> reader) {
    return reader ? EncodedInput(std::make_shared<EncodedInput::Impl>(std::move(reader))) : EncodedInput{};
}
std::shared_ptr<IByteRangeReader> EncodedInputAccess::Open(const EncodedInput& input) {
    if (!input.m_impl) { return {}; }
    return std::visit([](const auto& source) -> std::shared_ptr<IByteRangeReader> {
        using Source = std::decay_t<decltype(source)>;
        if constexpr (std::is_same_v<Source, std::filesystem::path>) {
            return std::make_shared<FileByteRangeReader>(source);
        } else if constexpr (std::is_same_v<Source, EncodedInput::Impl::Memory>) {
            return std::make_shared<MemoryByteRangeReader>(source.owner, source.bytes);
        } else if constexpr (std::is_same_v<Source, EncodedInput::Impl::Browser>) {
            auto reader = std::make_shared<wasm::WasmBrowserFileByteRangeReader>(source.id, source.size);
            struct Lifetime { std::shared_ptr<IByteRangeReader> reader; std::shared_ptr<const void> owner; };
            auto lifetime = std::make_shared<Lifetime>(Lifetime{reader, source.owner});
            return std::shared_ptr<IByteRangeReader>(std::move(lifetime), reader.get());
        } else {
            return source;
        }
    }, input.m_impl->source);
}

} // 命名空间 datacodec
