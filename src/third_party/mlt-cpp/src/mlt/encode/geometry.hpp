#pragma once

#include <mlt/encode/int.hpp>
#include <mlt/encoder.hpp>
#include <mlt/metadata/stream.hpp>
#include <mlt/metadata/tileset.hpp>
#include <mlt/util/encoding/buffer.hpp>
#include <mlt/util/encoding/varint.hpp>
#include <mlt/util/encoding/zigzag.hpp>
#include <mlt/util/hilbert_curve.hpp>
#include <mlt/util/morton_curve.hpp>
#include <mlt/util/stl.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <vector>

namespace mlt {
enum class IntegerEncodingOption : std::uint8_t;
} // namespace mlt

namespace mlt::encoder {

class GeometryEncoder {
public:
    using GeometryType = metadata::tileset::GeometryType;
    using PhysicalLevelTechnique = metadata::stream::PhysicalLevelTechnique;
    using PhysicalStreamType = metadata::stream::PhysicalStreamType;
    using LogicalStreamType = metadata::stream::LogicalStreamType;
    using LogicalLevelTechnique = metadata::stream::LogicalLevelTechnique;
    using DictionaryType = metadata::stream::DictionaryType;
    using LengthType = metadata::stream::LengthType;
    using OffsetType = metadata::stream::OffsetType;
    using StreamMetadata = metadata::stream::StreamMetadata;
    using RleEncodedStreamMetadata = metadata::stream::RleEncodedStreamMetadata;
    using MortonEncodedStreamMetadata = metadata::stream::MortonEncodedStreamMetadata;

    using EncodedChunks = std::vector<std::vector<std::uint8_t>>;

    struct EncodedGeometryColumn {
        std::uint32_t numStreams;
        EncodedChunks chunks;
        std::int32_t maxVertexValue;
    };

    struct Vertex {
        std::int32_t x;
        std::int32_t y;
        bool operator==(const Vertex&) const = default;
    };

    static EncodedGeometryColumn encodeGeometryColumn(std::span<const GeometryType> geometryTypes,
                                                      std::span<const std::uint32_t> numGeometries,
                                                      std::span<const std::uint32_t> numParts,
                                                      std::span<const std::uint32_t> numRings,
                                                      std::span<const Vertex> vertexBuffer,
                                                      PhysicalLevelTechnique physicalTechnique,
                                                      IntegerEncoder& intEncoder,
                                                      IntegerEncodingOption integerEncodingOption,
                                                      IntegerEncodingOption topologyIntegerEncodingOption,
                                                      bool enableMortonEncoding = true) {
        auto [numStreams, topologyChunks] = encodeTopologyStreams(geometryTypes,
                                                                  numGeometries,
                                                                  numParts,
                                                                  numRings,
                                                                  physicalTechnique,
                                                                  intEncoder,
                                                                  topologyIntegerEncodingOption);

        const auto [minVal, maxVal] = vertexBounds(vertexBuffer);

        auto plainEncoded = encodeVertexBufferPlain(vertexBuffer, physicalTechnique, intEncoder, integerEncodingOption);

        const auto enableHilbertEncoding = util::HilbertCurve::validCoordinateRange(minVal, maxVal);
        if (enableMortonEncoding && !util::MortonCurve::validCoordinateRange(minVal, maxVal)) {
            enableMortonEncoding = false;
        }

        if (integerEncodingOption != IntegerEncodingOption::AUTO && !enableMortonEncoding) {
            auto chunks = std::move(topologyChunks);
            util::appendChunks(chunks, std::move(plainEncoded));
            return {.numStreams = numStreams + 1, .chunks = std::move(chunks), .maxVertexValue = maxVal};
        }

        const MortonDictionary mortonDict = enableMortonEncoding ? buildMortonDictionary(vertexBuffer, minVal, maxVal)
                                                                 : MortonDictionary{};
        EncodedChunks mortonChunks = enableMortonEncoding
                                         ? encodeMortonDictionary(
                                               mortonDict, vertexBuffer, minVal, maxVal, physicalTechnique, intEncoder)
                                         : EncodedChunks{};
        const HilbertDictionary hilbertDict = enableHilbertEncoding
                                                  ? buildHilbertDictionary(vertexBuffer, minVal, maxVal)
                                                  : HilbertDictionary{};
        auto hilbertChunks =
            enableHilbertEncoding
                ? encodeHilbertDictionary(
                      hilbertDict, vertexBuffer, minVal, maxVal, physicalTechnique, intEncoder, integerEncodingOption)
                : EncodedChunks{};

        const auto plainSize = chunksSize(plainEncoded);
        const auto hilbertSize = chunksSize(hilbertChunks);
        const auto mortonSize = chunksSize(mortonChunks);

        auto result = std::move(topologyChunks);

        if (enableMortonEncoding && mortonSize < plainSize && mortonSize < hilbertSize) {
            util::appendChunks(result, std::move(mortonChunks));
            return {.numStreams = numStreams + 2, .chunks = std::move(result), .maxVertexValue = maxVal};
        }
        if (enableHilbertEncoding && hilbertSize < plainSize) {
            util::appendChunks(result, std::move(hilbertChunks));
            return {.numStreams = numStreams + 2, .chunks = std::move(result), .maxVertexValue = maxVal};
        }
        util::appendChunks(result, std::move(plainEncoded));
        return {.numStreams = numStreams + 1, .chunks = std::move(result), .maxVertexValue = maxVal};
    }

    static EncodedGeometryColumn encodePretessellatedGeometryColumn(std::span<const GeometryType> geometryTypes,
                                                                    std::span<const std::uint32_t> numGeometries,
                                                                    std::span<const std::uint32_t> numParts,
                                                                    std::span<const std::uint32_t> numRings,
                                                                    std::span<const Vertex> vertexBuffer,
                                                                    std::span<const std::uint32_t> numTriangles,
                                                                    std::span<const std::uint32_t> indexBuffer,
                                                                    PhysicalLevelTechnique physicalTechnique,
                                                                    IntegerEncoder& intEncoder,
                                                                    IntegerEncodingOption integerEncodingOption,
                                                                    IntegerEncodingOption topologyIntegerEncodingOption,
                                                                    bool encodeOutlines) {
        auto [numStreams, chunks] =
            encodeOutlines
                ? encodeTopologyStreams(geometryTypes,
                                        numGeometries,
                                        numParts,
                                        numRings,
                                        physicalTechnique,
                                        intEncoder,
                                        topologyIntegerEncodingOption)
                : encodeTopologyStreams(
                      geometryTypes, {}, {}, {}, physicalTechnique, intEncoder, topologyIntegerEncodingOption);

        appendUint32Stream(chunks,
                           numStreams,
                           numTriangles,
                           physicalTechnique,
                           intEncoder,
                           integerEncodingOption,
                           PhysicalStreamType::LENGTH,
                           LogicalStreamType{LengthType::TRIANGLES});

        appendUint32Stream(chunks,
                           numStreams,
                           indexBuffer,
                           physicalTechnique,
                           intEncoder,
                           integerEncodingOption,
                           PhysicalStreamType::OFFSET,
                           LogicalStreamType{OffsetType::INDEX});

        util::appendChunks(chunks,
                           encodeVertexBufferPlain(vertexBuffer, physicalTechnique, intEncoder, integerEncodingOption));
        ++numStreams;

        const auto maxVal = vertexBounds(vertexBuffer).second;

        return {.numStreams = numStreams, .chunks = std::move(chunks), .maxVertexValue = maxVal};
    }

private:
    struct TopologyResult {
        std::uint32_t numStreams;
        EncodedChunks chunks;
    };

    static std::size_t chunksSize(const EncodedChunks& chunks) {
        return util::sum(chunks, [](const auto& c) { return c.size(); });
    }

    static TopologyResult encodeTopologyStreams(std::span<const GeometryType> geometryTypes,
                                                std::span<const std::uint32_t> numGeometries,
                                                std::span<const std::uint32_t> numParts,
                                                std::span<const std::uint32_t> numRings,
                                                PhysicalLevelTechnique physicalTechnique,
                                                IntegerEncoder& intEncoder,
                                                IntegerEncodingOption integerEncodingOption) {
        std::vector<std::int32_t> geomTypeValues(geometryTypes.size());
        std::ranges::transform(
            geometryTypes, geomTypeValues.begin(), [](auto t) { return static_cast<std::int32_t>(t); });

        EncodedChunks chunks;
        util::appendChunks(chunks,
                           intEncoder.encodeIntStream(geomTypeValues,
                                                      physicalTechnique,
                                                      false,
                                                      PhysicalStreamType::LENGTH,
                                                      std::nullopt,
                                                      integerEncodingOption));
        std::uint32_t numStreams = 1;

        appendUint32Stream(chunks,
                           numStreams,
                           numGeometries,
                           physicalTechnique,
                           intEncoder,
                           integerEncodingOption,
                           PhysicalStreamType::LENGTH,
                           LogicalStreamType{LengthType::GEOMETRIES});

        auto partsEncodingOption = integerEncodingOption;
        if (integerEncodingOption == IntegerEncodingOption::AUTO && numParts.size() > 1) {
            const bool allMultiLine = std::ranges::all_of(geometryTypes,
                                                          [](auto t) { return t == GeometryType::MULTILINESTRING; });
            const bool repeatedPartLengths = std::ranges::all_of(numParts,
                                                                 [&](auto v) { return v == numParts.front(); });
            if (allMultiLine && repeatedPartLengths) {
                partsEncodingOption = IntegerEncodingOption::RLE;
            }
        }

        appendUint32Stream(chunks,
                           numStreams,
                           numParts,
                           physicalTechnique,
                           intEncoder,
                           partsEncodingOption,
                           PhysicalStreamType::LENGTH,
                           LogicalStreamType{LengthType::PARTS});
        appendUint32Stream(chunks,
                           numStreams,
                           numRings,
                           physicalTechnique,
                           intEncoder,
                           integerEncodingOption,
                           PhysicalStreamType::LENGTH,
                           LogicalStreamType{LengthType::RINGS});

        return {.numStreams = numStreams, .chunks = std::move(chunks)};
    }

    static void appendUint32Stream(EncodedChunks& chunks,
                                   std::uint32_t& numStreams,
                                   std::span<const std::uint32_t> values,
                                   PhysicalLevelTechnique physicalTechnique,
                                   IntegerEncoder& intEncoder,
                                   IntegerEncodingOption integerEncodingOption,
                                   PhysicalStreamType streamType,
                                   std::optional<LogicalStreamType> logicalType) {
        if (values.empty()) {
            return;
        }
        std::vector<std::int32_t> signedValues(values.size());
        std::ranges::transform(values, signedValues.begin(), [](auto v) { return static_cast<std::int32_t>(v); });
        util::appendChunks(
            chunks,
            intEncoder.encodeIntStream(
                signedValues, physicalTechnique, false, streamType, std::move(logicalType), integerEncodingOption));
        ++numStreams;
    }

    static std::pair<std::int32_t, std::int32_t> vertexBounds(std::span<const Vertex> vertexBuffer) {
        auto minVal = std::numeric_limits<std::int32_t>::max();
        auto maxVal = std::numeric_limits<std::int32_t>::min();
        for (const auto& v : vertexBuffer) {
            minVal = std::min({minVal, v.x, v.y});
            maxVal = std::max({maxVal, v.x, v.y});
        }
        return {minVal, maxVal};
    }

    static std::vector<std::int32_t> zigZagDeltaEncode(std::span<const Vertex> vertices) {
        std::vector<std::int32_t> result(vertices.size() * 2);
        Vertex prev{.x = 0, .y = 0};
        for (std::size_t i = 0; i < vertices.size(); ++i) {
            result[i * 2] = static_cast<std::int32_t>(util::encoding::encodeZigZag(vertices[i].x - prev.x));
            result[(i * 2) + 1] = static_cast<std::int32_t>(util::encoding::encodeZigZag(vertices[i].y - prev.y));
            prev = vertices[i];
        }
        return result;
    }

    static EncodedChunks encodeVertexBufferPlain(std::span<const Vertex> vertices,
                                                 PhysicalLevelTechnique physicalTechnique,
                                                 IntegerEncoder& intEncoder,
                                                 IntegerEncodingOption integerEncodingOption) {
        return encodeVertexBufferRaw(zigZagDeltaEncode(vertices), physicalTechnique, intEncoder, integerEncodingOption);
    }

    struct HilbertDictionary {
        std::vector<Vertex> vertices;
        std::vector<std::uint32_t> hilbertIds;
    };

    static HilbertDictionary buildHilbertDictionary(std::span<const Vertex> vertexBuffer,
                                                    std::int32_t minVal,
                                                    std::int32_t maxVal) {
        const util::HilbertCurve curve(minVal, maxVal);
        std::map<std::uint32_t, Vertex> dict;
        for (const auto& v : vertexBuffer) {
            const auto id = curve.encode({static_cast<float>(v.x), static_cast<float>(v.y)});
            dict.emplace(id, v);
        }
        HilbertDictionary result;
        result.vertices.reserve(dict.size());
        result.hilbertIds.reserve(dict.size());
        for (const auto& [id, v] : dict) {
            result.hilbertIds.push_back(id);
            result.vertices.push_back(v);
        }
        return result;
    }

    static EncodedChunks encodeHilbertDictionary(const HilbertDictionary& dict,
                                                 std::span<const Vertex> vertexBuffer,
                                                 std::int32_t minVal,
                                                 std::int32_t maxVal,
                                                 PhysicalLevelTechnique physicalTechnique,
                                                 IntegerEncoder& intEncoder,
                                                 IntegerEncodingOption integerEncodingOption) {
        const util::HilbertCurve curve(minVal, maxVal);
        const auto encodedDict = encodeVertexBufferRaw(
            zigZagDeltaEncode(dict.vertices), physicalTechnique, intEncoder, integerEncodingOption);
        return encodeDictionaryWithOffsets(
            vertexBuffer, dict.hilbertIds, curve, encodedDict, physicalTechnique, intEncoder);
    }

    struct MortonDictionary {
        std::vector<std::uint32_t> mortonCodes;
    };

    static MortonDictionary buildMortonDictionary(std::span<const Vertex> vertexBuffer,
                                                  std::int32_t minVal,
                                                  std::int32_t maxVal) {
        const util::MortonCurve curve(minVal, maxVal);
        std::set<std::uint32_t> codes;
        for (const auto& v : vertexBuffer) {
            codes.insert(curve.encode({static_cast<float>(v.x), static_cast<float>(v.y)}));
        }
        return {{codes.begin(), codes.end()}};
    }

    static EncodedChunks encodeMortonDictionary(const MortonDictionary& dict,
                                                std::span<const Vertex> vertexBuffer,
                                                std::int32_t minVal,
                                                std::int32_t maxVal,
                                                PhysicalLevelTechnique physicalTechnique,
                                                IntegerEncoder& intEncoder) {
        const util::MortonCurve curve(minVal, maxVal);
        const auto encodedDict = encodeMortonCodes(
            dict.mortonCodes, curve.getNumBits(), curve.getCoordinateShift(), physicalTechnique);
        return encodeDictionaryWithOffsets(
            vertexBuffer, dict.mortonCodes, curve, encodedDict, physicalTechnique, intEncoder);
    }

    static std::vector<std::int32_t> computeOffsets(std::span<const Vertex> vertexBuffer,
                                                    std::span<const std::uint32_t> sortedIds,
                                                    const util::SpaceFillingCurve& curve) {
        std::vector<std::int32_t> offsets;
        offsets.reserve(vertexBuffer.size());
        for (const auto& v : vertexBuffer) {
            const auto id = curve.encode({static_cast<float>(v.x), static_cast<float>(v.y)});
            const auto it = std::ranges::lower_bound(sortedIds, id);
            offsets.push_back(static_cast<std::int32_t>(it - sortedIds.begin()));
        }
        return offsets;
    }

    static EncodedChunks encodeDictionaryWithOffsets(std::span<const Vertex> vertexBuffer,
                                                     std::span<const std::uint32_t> sortedIds,
                                                     const util::SpaceFillingCurve& curve,
                                                     EncodedChunks encodedDict,
                                                     PhysicalLevelTechnique physicalTechnique,
                                                     IntegerEncoder& intEncoder) {
        const auto offsets = computeOffsets(vertexBuffer, sortedIds, curve);
        EncodedChunks result;
        util::appendChunks(
            result,
            intEncoder.encodeIntStream(
                offsets, physicalTechnique, false, PhysicalStreamType::OFFSET, LogicalStreamType{OffsetType::VERTEX}));
        util::appendChunks(result, std::move(encodedDict));
        return result;
    }

    static EncodedChunks encodeVertexBufferRaw(std::span<const std::int32_t> zigZagDelta,
                                               PhysicalLevelTechnique physicalTechnique,
                                               IntegerEncoder& intEncoder,
                                               IntegerEncodingOption integerEncodingOption) {
        auto encoded = intEncoder.encodeInt(zigZagDelta, physicalTechnique, /*isSigned=*/false, integerEncodingOption);

        // Match Java vertex metadata for plain zigzag-delta streams while preserving
        // non-plain logical transforms when they are explicitly selected.
        const bool isPlainZigZagDelta = (encoded.logicalLevelTechnique1 == LogicalLevelTechnique::NONE &&
                                         encoded.logicalLevelTechnique2 == LogicalLevelTechnique::NONE);
        const auto metadataLogical1 = isPlainZigZagDelta ? LogicalLevelTechnique::COMPONENTWISE_DELTA
                                                         : encoded.logicalLevelTechnique1;
        const auto metadataLogical2 = isPlainZigZagDelta ? LogicalLevelTechnique::NONE : encoded.logicalLevelTechnique2;

        std::vector<std::uint8_t> metadata;
        const bool isRle = (metadataLogical1 == LogicalLevelTechnique::RLE ||
                            metadataLogical2 == LogicalLevelTechnique::RLE);
        if (isRle) {
            metadata = RleEncodedStreamMetadata(PhysicalStreamType::DATA,
                                                LogicalStreamType{DictionaryType::VERTEX},
                                                metadataLogical1,
                                                metadataLogical2,
                                                physicalTechnique,
                                                encoded.physicalLevelEncodedValuesLength,
                                                static_cast<std::uint32_t>(encoded.encodedValues.size()),
                                                encoded.numRuns,
                                                static_cast<std::uint32_t>(zigZagDelta.size()))
                           .encode();
        } else {
            metadata = StreamMetadata(PhysicalStreamType::DATA,
                                      LogicalStreamType{DictionaryType::VERTEX},
                                      metadataLogical1,
                                      metadataLogical2,
                                      physicalTechnique,
                                      encoded.physicalLevelEncodedValuesLength,
                                      static_cast<std::uint32_t>(encoded.encodedValues.size()))
                           .encode();
        }

        return {std::move(metadata), std::move(encoded.encodedValues)};
    }

    static EncodedChunks encodeMortonCodes(std::span<const std::uint32_t> mortonCodes,
                                           std::uint32_t numBits,
                                           std::int32_t coordinateShift,
                                           PhysicalLevelTechnique physicalTechnique) {
        std::vector<std::int32_t> deltaValues(mortonCodes.size());
        std::int32_t prev = 0;
        for (std::size_t i = 0; i < mortonCodes.size(); ++i) {
            deltaValues[i] = static_cast<std::int32_t>(mortonCodes[i]) - prev;
            prev = static_cast<std::int32_t>(mortonCodes[i]);
        }

        std::vector<std::uint8_t> encodedData;
        encodedData.reserve(deltaValues.size() * 2);
        for (const auto v : deltaValues) {
            util::encoding::encodeVarint(static_cast<std::uint32_t>(v), encodedData);
        }

        auto metadata = MortonEncodedStreamMetadata(PhysicalStreamType::DATA,
                                                    LogicalStreamType{DictionaryType::MORTON},
                                                    LogicalLevelTechnique::MORTON,
                                                    LogicalLevelTechnique::DELTA,
                                                    physicalTechnique,
                                                    static_cast<std::uint32_t>(mortonCodes.size()),
                                                    static_cast<std::uint32_t>(encodedData.size()),
                                                    static_cast<int>(numBits),
                                                    static_cast<int>(coordinateShift))
                            .encode();

        return {std::move(metadata), std::move(encodedData)};
    }
};

} // namespace mlt::encoder
