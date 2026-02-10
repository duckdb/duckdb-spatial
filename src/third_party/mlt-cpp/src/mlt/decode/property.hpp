#pragma once

#include <mlt/decode/int.hpp>
#include <mlt/decode/string.hpp>
#include <mlt/feature.hpp>
#include <mlt/metadata/stream.hpp>
#include <mlt/metadata/tileset.hpp>
#include <mlt/properties.hpp>
#include <mlt/util/buffer_stream.hpp>
#include <mlt/util/packed_bitset.hpp>
#include <mlt/util/rle.hpp>
#include <mlt/util/raw.hpp>

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
    void skipColumn(BufferStream& tileData, std::uint32_t numStreams) {
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
        }

        const auto scalarColumn = std::get<ScalarColumn>(column.type);
        if (!scalarColumn.hasPhysicalType()) {
            throw std::runtime_error("property column ('" + column.name + "') must be scalar");
        }
        const auto scalarType = scalarColumn.getPhysicalType();

        // Everything but string has stream metadata.
        std::unique_ptr<StreamMetadata> streamMetadata;
        switch (scalarType) {
            case ScalarType::BOOLEAN:
            case ScalarType::INT_8:
            case ScalarType::UINT_8:
            case ScalarType::INT_32:
            case ScalarType::UINT_32:
            case ScalarType::INT_64:
            case ScalarType::UINT_64:
            case ScalarType::FLOAT:
            case ScalarType::DOUBLE:
                streamMetadata = StreamMetadata::decode(tileData);
                break;
            default:
            case ScalarType::STRING:
                // String always has present values stream.
                if (!presentValueCount) {
                    throw std::runtime_error("Missing present value column");
                }
                break;
        }

        if (streamMetadata && presentValueCount && presentValueCount < streamMetadata->getNumValues()) {
            throw std::runtime_error("Unexpected present value column");
        }

        const auto checkBits = [&](const auto& presentBuffer, const auto& propertyBuffer, bool isBoolean = false) {
            if (!presentStream.empty()) {
                const auto actualProperties = propertyCount(propertyBuffer, isBoolean);
                const auto presentBits = countSetBits(presentBuffer);
                if ((isBoolean && actualProperties / 8 != (presentBits + 7) / 8) ||
                    (!isBoolean && actualProperties != presentBits)) {
                    throw std::runtime_error("Property count " + std::to_string(actualProperties) +
                                             " doesn't match present bits " + std::to_string(presentBits));
                }
            }
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
                return {scalarType, std::move(result), std::move(presentStream)};
            }
            case ScalarType::INT_64: {
                std::vector<std::int64_t> longBuffer;
                longBuffer.reserve(streamMetadata->getNumValues());
                intDecoder.decodeIntStream<std::uint64_t, std::uint64_t, std::int64_t>(
                    tileData, longBuffer, *streamMetadata, /*isSigned=*/true);

                PropertyVec result{std::move(longBuffer)};
                checkBits(presentStream, result);
                return {scalarType, std::move(result), std::move(presentStream)};
            }
            case ScalarType::UINT_64: {
                std::vector<std::uint64_t> longBuffer;
                longBuffer.reserve(streamMetadata->getNumValues());
                intDecoder.decodeIntStream<std::uint64_t, std::uint64_t, std::uint64_t>(
                    tileData, longBuffer, *streamMetadata, /*isSigned=*/false);

                PropertyVec result{std::move(longBuffer)};
                checkBits(presentStream, result);
                return {scalarType, std::move(result), std::move(presentStream)};
            }
            case ScalarType::DOUBLE:
            case ScalarType::FLOAT: {
                const auto numValues = streamMetadata->getNumValues();
                const auto byteLength = streamMetadata->getByteLength();
                // Per spec, DOUBLE is 8 bytes and FLOAT is 4 bytes per value.
                // Java encoder writes 4-byte floats even for DOUBLE columns;
                // detect via byteLength and decode accordingly.
                if (byteLength == numValues * sizeof(double)) {
                    std::vector<double> doubleBuffer;
                    doubleBuffer.reserve(numValues);
                    decodeRaw(tileData, doubleBuffer, *streamMetadata, /*consume=*/true);

                    PropertyVec result{std::move(doubleBuffer)};
                    checkBits(presentStream, result);
                    return {scalarType, std::move(result), std::move(presentStream)};
                } else {
                    std::vector<float> floatBuffer;
                    floatBuffer.reserve(numValues);
                    decodeRaw(tileData, floatBuffer, *streamMetadata, /*consume=*/true);

                    PropertyVec result{std::move(floatBuffer)};
                    checkBits(presentStream, result);
                    return {scalarType, std::move(result), std::move(presentStream)};
                }
            }
            case ScalarType::STRING: {
                const auto stringCount = presentStream.empty() ? presentValueCount : countSetBits(presentStream);
                // FIXME? why numStreams - 1? what if its not optional
                auto strings = stringDecoder.decode(tileData, numStreams - 1, static_cast<std::uint32_t>(stringCount));

                PropertyVec result{std::move(strings)};
                checkBits(presentStream, result);
                return {scalarType, PropertyVec{std::move(result)}, std::move(presentStream)};
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
