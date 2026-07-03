#pragma once

#include <mlt/common.hpp>
#include <mlt/decode/int.hpp>
#include <mlt/metadata/stream.hpp>
#include <mlt/metadata/tileset.hpp>
#include <mlt/properties.hpp>
#include <mlt/util/buffer_stream.hpp>
#include <mlt/util/packed_bitset.hpp>
#include <mlt/util/raw.hpp>
#include <mlt/util/varint.hpp>

#include <stdexcept>
#include <string>
#include <string_view>

namespace mlt::decoder {

class StringDecoder {
public:
    StringDecoder(IntegerDecoder& intDecoder_)
        : intDecoder(intDecoder_) {}

    /*
     * String column layouts:
     * -> plain -> present, length, data
     * -> dictionary -> present, length, dictionary, data
     * -> fsst dictionary -> symbolTable, symbolLength, dictionary, length, present, data
     * */

    StringDictViews decode(BufferStream& tileData, std::uint32_t numStreams) {
        using namespace metadata::stream;
        using namespace util::decoding;

        std::optional<DictionaryType> dictType;
        std::optional<LengthType> lengthType;
        std::vector<std::uint8_t> dictionaryStream;
        std::vector<std::uint8_t> symbolStream;
        std::vector<std::uint32_t> offsetStream;
        std::vector<std::uint32_t> dictLengthStream;   // NOLINT(misc-const-correctness)
        std::vector<std::uint32_t> symbolLengthStream; // NOLINT(misc-const-correctness)
        std::vector<std::string_view> views;
        for (std::uint32_t i = 0; i < numStreams; ++i) {
            const auto streamMetadata = StreamMetadata::decode(tileData);
            switch (streamMetadata->getPhysicalStreamType()) {
                case PhysicalStreamType::OFFSET:
                    intDecoder.decodeIntStream<std::uint32_t>(tileData, offsetStream, *streamMetadata);
                    break;
                case PhysicalStreamType::LENGTH: {
                    if (!streamMetadata->getLogicalStreamType() ||
                        !streamMetadata->getLogicalStreamType()->getLengthType()) {
                        throw std::runtime_error("Length stream missing logical type");
                    }
                    lengthType = streamMetadata->getLogicalStreamType()->getLengthType();
                    auto& target = (lengthType == LengthType::DICTIONARY) ? dictLengthStream : symbolLengthStream;
                    intDecoder.decodeIntStream<std::uint32_t>(tileData, target, *streamMetadata);
                    break;
                }
                case PhysicalStreamType::DATA: {
                    if (!streamMetadata->getLogicalStreamType() ||
                        !streamMetadata->getLogicalStreamType()->getDictionaryType()) {
                        throw std::runtime_error("Data stream missing logical type");
                    }
                    dictType = streamMetadata->getLogicalStreamType()->getDictionaryType();
                    auto& target = (dictType == DictionaryType::SINGLE) ? dictionaryStream : symbolStream;
                    decodeRaw(tileData, target, streamMetadata->getByteLength(), /*consume=*/true);
                    break;
                }
                default:
                    throw std::runtime_error("Unsupported stream type");
            }
        }

        if (!dictLengthStream.empty() && !symbolLengthStream.empty()) {
            auto data = decodeFSST(symbolStream, symbolLengthStream, dictionaryStream, 2 * dictionaryStream.size());
            decodeDictionary(dictLengthStream, data, offsetStream, views);
            return {std::move(data), std::move(views)};
        }
        if (!offsetStream.empty() && !dictLengthStream.empty()) {
            decodeDictionary(dictLengthStream, dictionaryStream, offsetStream, views);
            return {std::move(dictionaryStream), std::move(views)};
        }
        if (!symbolLengthStream.empty()) {
            decodePlain(symbolLengthStream, symbolStream, views);
            return {std::move(symbolStream), std::move(views)};
        }
        throw std::runtime_error("Expected streams missing in string decoding");
    }

    /// Multiple string columns sharing a dictionary
    PropertyVecMap decodeSharedDictionary(BufferStream& tileData,
                                          const metadata::tileset::Column& column,
                                          std::uint32_t numStreams) {
        using namespace metadata::stream;
        using namespace metadata::tileset::schema;
        using namespace util::decoding;

        if (!column.hasComplexType() || !column.getComplexType().hasChildren() || numStreams < 3) {
            throw std::runtime_error("Expected struct column for shared dictionary decoding");
        }

        std::vector<std::uint32_t> dictionaryLengthStream;
        auto dictionaryStream = std::make_shared<std::vector<std::uint8_t>>();
        std::vector<std::uint32_t> symbolLengthStream;
        std::vector<std::uint8_t> symbolTableStream;
        PropertyVecMap results;

        bool dictionaryStreamDecoded = false;
        while (!dictionaryStreamDecoded && numStreams--) {
            const auto streamMetadata = metadata::stream::StreamMetadata::decode(tileData);
            switch (streamMetadata->getPhysicalStreamType()) {
                case PhysicalStreamType::LENGTH: {
                    const bool isDict = streamMetadata->getLogicalStreamType()->getLengthType() ==
                                        LengthType::DICTIONARY;
                    auto& target = isDict ? dictionaryLengthStream : symbolLengthStream;
                    intDecoder.decodeIntStream<std::uint32_t>(tileData, target, *streamMetadata);
                    break;
                }
                case PhysicalStreamType::DATA: {
                    const auto type = streamMetadata->getLogicalStreamType()->getDictionaryType();
                    const auto isDict = (type == DictionaryType::SINGLE || type == DictionaryType::SHARED);
                    auto& targetStream = isDict ? *dictionaryStream : symbolTableStream;
                    decodeRaw(tileData, targetStream, streamMetadata->getByteLength(), /*consume=*/true);
                    dictionaryStreamDecoded = isDict;
                    break;
                }
                default:
                    throw std::runtime_error("Unsupported stream type");
            }
        }

        std::vector<std::string_view> dictionaryViews;
        if (!symbolLengthStream.empty() && !symbolTableStream.empty() && !dictionaryLengthStream.empty()) {
            auto data = std::make_shared<std::vector<std::uint8_t>>(
                decodeFSST(symbolTableStream, symbolLengthStream, *dictionaryStream, 2 * dictionaryStream->size()));
            decodeDictionary(dictionaryLengthStream, *data, dictionaryViews);
            // retain the decoded dictionary data instead of the original raw data
            dictionaryStream.swap(data);
        } else if (!dictionaryLengthStream.empty() && !dictionaryStream->empty()) {
            decodeDictionary(dictionaryLengthStream, *dictionaryStream, dictionaryViews);
        } else {
            throw std::runtime_error("Expected streams missing in shared dictionary decoding");
        }

        for (const auto& child : column.getComplexType().children) {
            [[maybe_unused]] const auto childStreams = decodeVarint<std::uint32_t>(tileData);

            if (!child.hasScalarType() || child.getScalarType().getPhysicalType() != ScalarType::STRING) {
                throw std::runtime_error("Currently only string fields are implemented for a struct");
            }

            if (childStreams == 0) {
                // column with all null values, skip
                results.emplace(column.name + child.name, PresentProperties{ScalarType::STRING, {}, {}});
                continue;
            }

            PackedBitset presentStream;
            if (child.nullable) {
                const auto presentStreamMetadata = metadata::stream::StreamMetadata::decode(tileData);
                const auto presentValueCount = presentStreamMetadata->getNumValues();
                rle::decodeBoolean(tileData, presentStream, *presentStreamMetadata, /*consume=*/true);
                if ((presentValueCount + 7) / 8 != presentStream.size()) {
                    throw std::runtime_error("invalid present stream");
                }
            }

            const auto dataStreamMetadata = metadata::stream::StreamMetadata::decode(tileData);
            std::vector<std::uint32_t> dataReferenceStream;
            intDecoder.decodeIntStream<std::uint32_t>(tileData, dataReferenceStream, *dataStreamMetadata);

            std::vector<std::string_view> propertyValues;
            propertyValues.reserve(dataReferenceStream.size());

            for (auto dictIndex : dataReferenceStream) {
                if (dictIndex >= dictionaryViews.size()) {
                    throw std::runtime_error("StringDecoder: dictionaryViews index out of bounds");
                }
                propertyValues.push_back(dictionaryViews[dictIndex]);
            }

            results.emplace(column.name + child.name,
                            PresentProperties{child.getScalarType().getPhysicalType(),
                                              StringDictViews(dictionaryStream, propertyValues),
                                              presentStream});
        }

        return results;
    }

    static std::vector<std::uint8_t> decodeFSST(const std::vector<std::uint8_t>& symbols,
                                                const std::vector<std::uint32_t>& symbolLengths,
                                                const std::vector<std::uint8_t>& compressedData,
                                                std::size_t decompressedLength) {
        return decodeFSST(symbols.data(),
                          symbols.size(),
                          symbolLengths.data(),
                          symbolLengths.size(),
                          compressedData.data(),
                          compressedData.size(),
                          decompressedLength);
    }

    static std::vector<std::uint8_t> decodeFSST(const std::uint8_t* symbols,
                                                const std::size_t symbolCount,
                                                const std::uint32_t* symbolLengths,
                                                const std::size_t symbolLengthCount,
                                                const std::uint8_t* compressedData,
                                                const std::size_t compressedDataCount,
                                                const std::size_t decompressedLength) {
        std::vector<std::uint8_t> output;

        if (decompressedLength > 0) {
            output.resize(decompressedLength);
        }
        std::vector<std::uint32_t> symbolOffsets(symbolLengthCount);
        for (size_t i = 1; i < symbolLengthCount; i++) {
            symbolOffsets[i] = symbolOffsets[i - 1] + symbolLengths[i - 1];
        }

        std::size_t idx = 0;
        for (size_t i = 0; i < compressedDataCount; i++) {
            const std::uint8_t symbolIndex = compressedData[i];

            // 255 is our escape byte -> take the next symbol as it is
            if (symbolIndex == 255) {
                if (idx == output.size()) {
                    output.resize(output.size() * 2);
                }
                output[idx++] = compressedData[++i];
            } else if (symbolIndex < symbolLengthCount) {
                const auto len = symbolLengths[symbolIndex];
                if (idx + len > output.size()) {
                    output.resize((output.size() + len) * 2);
                }
                const auto offset = symbolOffsets[symbolIndex];
                if (offset >= symbolCount) {
                    throw std::runtime_error("FSST decode: symbol index out of bounds");
                }
                std::memcpy(&output[idx], &symbols[offset], len);
                idx += len;
            } else {
                throw std::runtime_error("FSST decode: invalid symbol index");
            }
        }

        output.resize(idx);
        return output;
    }

private:
    IntegerDecoder& intDecoder;

    /// Drop the useless codepoint produced when a UTF-16 Byte-Order-Mark is included in the conversion to UTF-8
    // https://en.wikipedia.org/wiki/Byte_order_mark#UTF-8
    // TODO: Can we force this on the encoding side instead?
    static std::string_view view(const char* bytes, std::size_t length) {
        if (length >= 3 && std::equal(bytes, bytes + 3, "\xEF\xBB\xBF")) {
            bytes += 3;
            length -= 3;
        }
        return {bytes, length};
    }

    static void decodePlain(const std::vector<std::uint32_t>& lengthStream,
                            const std::vector<std::uint8_t>& utf8bytes,
                            std::vector<std::string_view>& out) {
        std::size_t dataOffset = 0;
        std::size_t lengthOffset = 0;
        out.reserve(lengthStream.size());
        for (std::uint32_t i = 0; i < lengthStream.size(); ++i) {
            const auto length = lengthStream[lengthOffset++];
            const char* bytes = reinterpret_cast<std::string::const_pointer>(utf8bytes.data() + dataOffset);
            out.push_back(view(bytes, length));
            dataOffset += length;
        }
    }

    static void decodeDictionary(const std::vector<std::uint32_t>& lengthStream,
                                 const std::vector<std::uint8_t>& utf8bytes,
                                 std::vector<std::string_view>& out) {
        const auto* const utf8Ptr = reinterpret_cast<const char*>(utf8bytes.data());

        std::size_t offset = 0;
        for (auto length : lengthStream) {
            out.emplace_back(utf8Ptr + offset, length);
            offset += length;
        }
    }

    static void decodeDictionary(const std::vector<std::uint32_t>& lengthStream,
                                 const std::vector<std::uint8_t>& utf8bytes,
                                 const std::vector<std::uint32_t>& offsets,
                                 std::vector<std::string_view>& out) {
        const auto* const utf8Ptr = reinterpret_cast<const char*>(utf8bytes.data());

        std::vector<std::string_view> dictionary;
        dictionary.reserve(lengthStream.size());

        std::uint32_t dictionaryOffset = 0;
        for (const auto length : lengthStream) {
            dictionary.push_back(view(utf8Ptr + dictionaryOffset, length));
            dictionaryOffset += length;
        }

        out.reserve(offsets.size());
        for (const auto offset : offsets) {
            out.push_back(dictionary[offset]);
        }
    }
};

} // namespace mlt::decoder
