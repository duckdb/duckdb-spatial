#pragma once

#include <algorithm>
#include <mlt/encode/boolean.hpp>
#include <mlt/encode/float.hpp>
#include <mlt/encode/int.hpp>
#include <mlt/encode/string.hpp>
#include <mlt/metadata/stream.hpp>
#include <mlt/metadata/tileset.hpp>
#include <mlt/util/encoding/varint.hpp>
#include <mlt/util/stl.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace mlt::encoder {

class PropertyEncoder {
public:
    using ScalarType = metadata::tileset::ScalarType;
    using PhysicalLevelTechnique = metadata::stream::PhysicalLevelTechnique;
    using PhysicalStreamType = metadata::stream::PhysicalStreamType;
    using EncodedChunks = std::vector<std::vector<std::uint8_t>>;

    template <typename T, typename EncodeDataFn>
    static EncodedChunks encodeDataColumn(std::span<const T> dataValues,
                                          const std::vector<bool>& presentValues,
                                          bool hasNull,
                                          bool columnNullable,
                                          EncodeDataFn&& encodeData) {
        EncodedChunks chunks;
        chunks.reserve(2);
        appendPresentStream(chunks, presentValues, hasNull, columnNullable);

        auto dataStream = encodeData(dataValues);
        if constexpr (std::same_as<std::remove_cvref_t<decltype(dataStream)>, EncodedChunks>) {
            util::appendChunks(chunks, std::move(dataStream));
        } else {
            chunks.push_back(std::move(dataStream));
        }
        return chunks;
    }

    /// Unsigned integers are encoded separately to avoid undefined behvior when casting unsigned values outside the
    /// range of signed integers.

    static EncodedChunks encodeInt32Column(std::span<const std::optional<std::int32_t>> values,
                                           PhysicalLevelTechnique physicalTechnique,
                                           bool isSigned,
                                           IntegerEncoder& intEncoder,
                                           bool columnNullable = false) {
        return encodeColumn<std::int32_t>(values, columnNullable, [&](std::span<const std::int32_t> dataValues) {
            return intEncoder.encodeIntStream(
                dataValues, physicalTechnique, isSigned, metadata::stream::PhysicalStreamType::DATA, std::nullopt);
        });
    }

    static EncodedChunks encodeUint64Column(std::span<const std::optional<std::uint64_t>> values,
                                            IntegerEncoder& intEncoder,
                                            bool columnNullable = false) {
        return encodeColumn<std::uint64_t>(values, columnNullable, [&](std::span<const std::uint64_t> dataValues) {
            return intEncoder.encodeUint64Stream(dataValues, metadata::stream::PhysicalStreamType::DATA, std::nullopt);
        });
    }

    static EncodedChunks encodeStringColumn(std::span<const std::string_view> dataValues,
                                            const std::vector<bool>& presentValues,
                                            PhysicalLevelTechnique physicalTechnique,
                                            IntegerEncoder& intEncoder,
                                            bool useFsst = true,
                                            bool columnNullable = false) {
        auto stringResult = StringEncoder::encode(dataValues, physicalTechnique, intEncoder, useFsst);
        const auto streamCount = stringResult.numStreams + (columnNullable ? 1 : 0);

        EncodedChunks chunks;
        chunks.reserve(3);
        std::vector<std::uint8_t> streamCountBytes;
        util::encoding::encodeVarint(streamCount, streamCountBytes);
        chunks.push_back(std::move(streamCountBytes));
        if (columnNullable) {
            auto presentStream = BooleanEncoder::encodeBooleanStream(presentValues, PhysicalStreamType::PRESENT);
            chunks.push_back(std::move(presentStream));
        }
        util::appendChunks(chunks, std::move(stringResult.chunks));
        return chunks;
    }

private:
    template <typename T>
    // Represents an optional column split into a present bitmap, compacted non-null values,
    // and a fast flag indicating whether any null was observed.
    struct SeparatedNulls {
        std::vector<bool> present;
        std::vector<T> data;
        bool hasNull = false;
    };

    template <typename T>
    // Converts optional values into SeparatedNulls so downstream encoders can process
    // present and data streams independently without re-scanning the input.
    static SeparatedNulls<T> separateNulls(std::span<const std::optional<T>> values) {
        SeparatedNulls<T> result;
        result.present.reserve(values.size());
        result.data.reserve(values.size());
        for (const auto& v : values) {
            if (v.has_value()) {
                result.present.push_back(true);
                result.data.push_back(*v);
            } else {
                result.present.push_back(false);
                result.hasNull = true;
            }
        }
        return result;
    }

    static void appendPresentStream(EncodedChunks& chunks,
                                    const std::vector<bool>& presentValues,
                                    bool hasNull,
                                    bool columnNullable = false) {
        if (hasNull || columnNullable) {
            auto presentStream = BooleanEncoder::encodeBooleanStream(presentValues, PhysicalStreamType::PRESENT);
            chunks.push_back(std::move(presentStream));
        }
    }

    template <typename T, typename EncodeDataFn>
    static EncodedChunks encodeColumn(std::span<const std::optional<T>> values,
                                      bool columnNullable,
                                      EncodeDataFn&& encodeData) {
        const auto [presentValues, dataValues, hasNull] = separateNulls<T>(values);
        return encodeDataColumn(std::span<const T>{dataValues}, presentValues, hasNull, columnNullable, encodeData);
    }
};

} // namespace mlt::encoder
