#pragma once

#include "DataCodec/Test/Adapter/DataCodecTestAdapter.h"
#include "DataCodec/API/Entry/DataCodecEncodeEntry.h"
#include "DataCodec/API/Entry/DataCodecDecodeEntry.h"
#include "DataCodec/Storage/LeafPackage/LeafPackageIO.h"
#include "DataCodec/Storage/ByteIO/FileByteRangeIO.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <zstd.h>

namespace datacodec::test {
inline float DecodeFixtureValue(std::size_t point, std::size_t field) {
    const auto bits = static_cast<std::uint32_t>(point) * 1664525u + static_cast<std::uint32_t>(field) * 1013904223u;
    return static_cast<float>((bits >> 8u) & 65535u) / 1024.0f;
}

inline int MakeDecodeFixture(const char* path, std::size_t points, std::size_t fields, bool outer) {
    if (std::filesystem::exists(path) || points == 0u || points > (1u << 23u) || fields == 0u || fields > 512u) { return 2; }
    auto data = std::make_shared<TestDataset>();
    data->name = "decode_fixture";
    data->points.resize(points * 3u);
    for (std::size_t i = 0; i < points; ++i) { data->points[i * 3u] = static_cast<float>(i); }
    for (std::size_t j = 0; j < fields; ++j) {
        TestNumericField field{.name = "field_" + std::to_string(j), .role = AttrRole::Scalar};
        field.values.resize(points);
        for (std::size_t i = 0; i < points; ++i) { field.values[i] = DecodeFixtureValue(i, j); }
        data->pointFields.push_back(std::move(field));
    }
    auto configuration = MakeDefaultEncodeConfigurationParams();
    configuration.controlParams.attrReference.intraField.codec = IntraFieldReferenceCodec::Disabled;
    configuration.pipelineControl.packageFields.mode = PackageFieldEncodingMode::Raw;
    auto encoded = Encode({.input = EncodeInput::LeafAdapter(std::make_shared<TestEncodeAdapter>(*data)),
        .output = EncodeOutput::Memory(EncodePackageKind::LeafPackage), .configuration = configuration,
        .resources = {.mode = CodecResourceMode::Unlimited}});
    if (!encoded.success) { std::cerr << FormatCodecFailure(*encoded.failure) << '\n'; return 3; }
    data.reset();
    auto reader = std::make_shared<MemoryByteRangeReader>(std::make_shared<const EncodedBuffer>(std::move(encoded.encodedBytes)));
    LeafPackage leaf;
    std::string error;
    if (!LeafPackageIO::ReadFromByteRange(reader, 0u, reader->ByteSize(), leaf, &error)) { std::cerr << error; return 4; }
    if (outer) {
        for (auto& field : leaf.fields) {
            if (field.type != FieldType::Attribute) { continue; }
            std::vector<std::uint8_t> raw(field.rawSize), compressed(ZSTD_compressBound(field.rawSize));
            if (!field.source->Read(0u, raw, &error)) { std::cerr << error; return 4; }
            const auto bytes = ZSTD_compress(compressed.data(), compressed.size(), raw.data(), raw.size(), 1);
            if (ZSTD_isError(bytes)) { return 4; }
            compressed.resize(bytes);
            field.source = std::make_shared<bytestore::VectorByteSource>(std::move(compressed));
            field.compressionType = EncodedFieldCompressionType::ZSTD;
            std::cout << "FIXTURE outer_raw=" << raw.size() << " outer_compressed=" << bytes << '\n';
        }
    }
    class Writer final : public bytestore::IByteWriter {
    public:
        explicit Writer(const char* path) : file(path, std::ios::binary) {}
        bool Write(std::span<const std::uint8_t> bytes, std::string*) override {
            file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()); return bool(file);
        }
        std::ofstream file;
    } writer(path);
    if (!LeafPackageIO::WriteLeafPackage(leaf, writer, &error)) { std::cerr << error; return 4; }
    writer.file.close();
    return writer.file ? 0 : 4;
}

inline int RunDecodeFixture(const char* path, std::size_t repeats) {
    if (repeats == 0u || repeats > 1000u) { return 2; }
    double seconds = 0.0;
    for (std::size_t repeat = 0; repeat < repeats; ++repeat) {
        const auto start = std::chrono::steady_clock::now();
        auto decoded = DecodePackage({.input = EncodedInput::File(path), .resources = {.mode = CodecResourceMode::Unlimited}});
        seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (!decoded.success) { if(decoded.failure) std::cerr << FormatCodecFailure(*decoded.failure); return 5; }
        if (repeat == 0u) {
            if (decoded.output.leaves.size() != 1u) { return 6; }
            const auto& leaf = decoded.output.leaves.front();
            const auto* points = reinterpret_cast<const float*>(leaf.geometry.values.data());
            for (const auto& field : leaf.attributes) {
                if (field.values.size() != leaf.geometry.pointCount * sizeof(float)) { return 6; }
                const auto* values = reinterpret_cast<const float*>(field.values.data());
                for (std::size_t i = 0; i < leaf.geometry.pointCount; ++i) {
                    if (values[i] != DecodeFixtureValue(static_cast<std::size_t>(points[i * 3u]), field.sourceIndex)) { return 6; }
                }
            }
            std::cout << "VERIFIED points=" << leaf.geometry.pointCount << " fields=" << leaf.attributes.size() << '\n';
        }
    }
    std::cout << "RESULT decode_success=1 decode_seconds=" << seconds / repeats << " repeats=" << repeats << '\n';
    return 0;
}
}
