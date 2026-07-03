#pragma once

#include <algorithm>
#include <iterator>
#include <mlt/decode/int.hpp>
#include <mlt/decode/string.hpp>
#include <mlt/feature.hpp>
#include <mlt/metadata/stream.hpp>
#include <mlt/metadata/tileset.hpp>
#include <mlt/properties.hpp>
#include <mlt/util/buffer_stream.hpp>
#include <mlt/util/packed_bitset.hpp>
#include <mlt/util/raw.hpp>
#include <mlt/util/rle.hpp>

#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace mlt::decoder {

class PropertyDecoder {
public:
    PropertyDecoder(IntegerDecoder& intDecoder_, StringDecoder& stringDecoder_)
        : intDecoder(intDecoder_),
          stringDecoder(stringDecoder_) {}

    PropertyVecMap decodePropertyColumn(BufferStream& tileData,
                                        const metadata::tileset::Column& column,
                                        std::uint32_t numStreams) {
        using namespace metadata::tileset;
        if (std::holds_alternative<ScalarColumn>(column.type)) {
            PropertyVecMap results;
            results.emplace(column.name, decodeScalarPropertyColumn(tileData, column, numStreams));
            return results;
        }

        if (numStreams > 1) {
            return stringDecoder.decodeSharedDictionary(tileData, column, numStreams);
        }

        skipColumn(tileData, numStreams);
        return {};
    }

protected:
    static void skipColumn(BufferStream& tileData, std::uint32_t numStreams) {
        using namespace metadata::stream;
        using namespace util::decoding;

        for (std::uint32_t i = 0; i < numStreams; ++i) {
            auto streamMetadata = StreamMetadata::decode(tileData);
            if (!streamMetadata) {
                throw std::runtime_error("Failed to decode stream metadata");
            }

            // Skip the stream data
            tileData.consume(streamMetadata->getByteLength());
        }
    }

    PresentProperties decodeScalarPropertyColumn(BufferStream& tileData,
                                                 const metadata::tileset::Column& column,
                                                 std::uint32_t numStreams) {
        using namespace metadata;
        using namespace metadata::stream;
        using namespace metadata::tileset;
        using namespace util::decoding;

        PackedBitset presentStream;
        std::uint32_t presentValueCount = 0;

        if (column.nullable) {
            const auto presentStreamMetadata = StreamMetadata::decode(tileData);
            presentValueCount = presentStreamMetadata->getNumValues();
            rle::decodeBoolean(tileData, presentStream, *presentStreamMetadata, /*consume=*/true);
            if ((presentValueCount + 7) / 8 != presentStream.size()) {
                throw std::runtime_error("invalid present stream");
            }
            numStreams -= 1;
        }

        const auto scalarColumn = std::get<ScalarColumn>(column.type);
        if (!scalarColumn.hasPhysicalType()) {
            throw std::runtime_error("property column ('" + column.name + "') must be scalar");
        }
        const auto scalarType = scalarColumn.getPhysicalType();

        // String stream metadata is read by StringDecoder
        std::unique_ptr<StreamMetadata> streamMetadata;
        if (scalarType != ScalarType::STRING) {
            streamMetadata = StreamMetadata::decode(tileData);
        }

        // A single-value RLE run encodes as more ints than it decodes to.
        // Compare the logical (decoded) count, not the raw stream length, to avoid a spurious throw.
        if (column.nullable && streamMetadata &&
            presentValueCount <
                IntegerDecoder::getIntArrayBufferSize(streamMetadata->getNumValues(), *streamMetadata)) {
            throw std::runtime_error("Unexpected present value column");
        }

        const auto checkBits = [&](const auto& presentBuffer, const auto& propertyBuffer, bool isBoolean = false) {
#ifndef NDEBUG
            if (!presentStream.empty()) {
                const auto actualProperties = propertyCount(propertyBuffer, isBoolean);
                const auto presentBits = countSetBits(presentBuffer);
                if ((isBoolean && actualProperties / 8 != (presentBits + 7) / 8) ||
                    (!isBoolean && actualProperties != presentBits)) {
                    throw std::runtime_error("Property count " + std::to_string(actualProperties) +
                                             " doesn't match present bits " + std::to_string(presentBits));
                }
            }
#endif
        };

        switch (scalarType) {
            case ScalarType::BOOLEAN: {
                std::vector<std::uint8_t> byteBuffer;
                rle::decodeBoolean(tileData, byteBuffer, *streamMetadata, /*consume=*/true);
                if (streamMetadata->getNumValues() > 0 &&
                    (streamMetadata->getNumValues() + 7) / 8 != byteBuffer.size()) {
                    throw std::runtime_error("column data incomplete");
                }

                checkBits(presentStream, byteBuffer, /*isBoolean=*/true);
                return {scalarType, byteBuffer, presentStream};
            }
            case ScalarType::INT_8:
            case ScalarType::UINT_8:
                throw std::runtime_error("8-bit integer type not implemented");
            case ScalarType::INT_32:
            case ScalarType::UINT_32: {
                const bool isSigned = (scalarType == ScalarType::INT_32);
                PropertyVec result;
                if (isSigned) {
                    std::vector<std::int32_t> intBuffer;
                    intBuffer.reserve(streamMetadata->getNumValues());
                    intDecoder.decodeIntStream<std::uint32_t, std::uint32_t, std::int32_t>(
                        tileData, intBuffer, *streamMetadata, isSigned);
                    result = {std::move(intBuffer)};
                } else {
                    std::vector<std::uint32_t> uintBuffer;
                    uintBuffer.reserve(streamMetadata->getNumValues());
                    intDecoder.decodeIntStream<std::uint32_t, std::uint32_t, std::uint32_t>(
                        tileData, uintBuffer, *streamMetadata, isSigned);
                    result = {std::move(uintBuffer)};
                }

                checkBits(presentStream, result);
                return {scalarType, std::move(result), presentStream};
            }
            case ScalarType::INT_64: {
                std::vector<std::int64_t> longBuffer;
                longBuffer.reserve(streamMetadata->getNumValues());
                intDecoder.decodeIntStream<std::uint64_t, std::uint64_t, std::int64_t>(
                    tileData, longBuffer, *streamMetadata, /*isSigned=*/true);

                PropertyVec result{std::move(longBuffer)};
                checkBits(presentStream, result);
                return {scalarType, std::move(result), presentStream};
            }
            case ScalarType::UINT_64: {
                std::vector<std::uint64_t> longBuffer;
                longBuffer.reserve(streamMetadata->getNumValues());
                intDecoder.decodeIntStream<std::uint64_t, std::uint64_t, std::uint64_t>(
                    tileData, longBuffer, *streamMetadata, /*isSigned=*/false);

                PropertyVec result{std::move(longBuffer)};
                checkBits(presentStream, result);
                return {scalarType, std::move(result), presentStream};
            }
            case ScalarType::DOUBLE: {
                std::vector<double> doubleBuffer;
                if (streamMetadata->getNumValues() * sizeof(double) == streamMetadata->getByteLength()) {
                    decodeRaw(tileData, doubleBuffer, *streamMetadata, /*consume=*/true);
                } else {
                    // Compatibility with tilesets encoded before double support was added
                    std::vector<float> floatBuffer;
                    decodeRaw(tileData, floatBuffer, *streamMetadata, /*consume=*/true);
                    doubleBuffer.reserve(streamMetadata->getNumValues());
                    std::ranges::transform(floatBuffer, std::back_inserter(doubleBuffer), [](float value) {
                        return static_cast<double>(value);
                    });
                }

                PropertyVec result{std::move(doubleBuffer)};
                checkBits(presentStream, result);
                return {scalarType, std::move(result), presentStream};
            }
            case ScalarType::FLOAT: {
                std::vector<float> floatBuffer;
                decodeRaw(tileData, floatBuffer, *streamMetadata, /*consume=*/true);

                PropertyVec result{std::move(floatBuffer)};
                checkBits(presentStream, result);
                return {scalarType, std::move(result), presentStream};
            }
            case ScalarType::STRING: {
                auto strings = stringDecoder.decode(tileData, numStreams);
                if (column.nullable && countSetBits(presentStream) != strings.getStrings().size()) {
                    throw std::runtime_error("String count doesn't match present value count");
                }

                PropertyVec result{std::move(strings)};
                checkBits(presentStream, result);
                return {scalarType, PropertyVec{std::move(result)}, presentStream};
            }
            default:
                throw std::runtime_error("Unknown scalar type: " + std::to_string(std::to_underlying(scalarType)));
        }
    }

private:
    IntegerDecoder& intDecoder;
    StringDecoder& stringDecoder;
};

} // namespace mlt::decoder
