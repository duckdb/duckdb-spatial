#pragma once

#include <mlt/decode/int.hpp>
#include <mlt/feature.hpp>
#include <mlt/geometry.hpp>
#include <mlt/geometry_vector.hpp>
#include <mlt/metadata/stream.hpp>
#include <mlt/metadata/tileset.hpp>
#include <mlt/util/buffer_stream.hpp>
#include <mlt/util/noncopyable.hpp>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mlt::decoder {

class GeometryDecoder {
public:
    using GeometryVector = geometry::GeometryVector;

private:
    enum class VectorType : std::uint32_t {
        FLAT,
        CONST,
        SEQUENCE,
        DICTIONARY,
        FSST_DICTIONARY,
    };

    VectorType getVectorTypeIntStream(const metadata::stream::StreamMetadata& streamMetadata) {
        using namespace metadata::stream;
        const auto logicalLevelTechnique1 = streamMetadata.getLogicalLevelTechnique1();
        const auto logicalLevelTechnique2 = streamMetadata.getLogicalLevelTechnique2();
        const auto metadataType = streamMetadata.getMetadataType();
        const auto& rleMetadata = static_cast<const RleEncodedStreamMetadata&>(streamMetadata);
        const auto rleRuns = (metadataType == LogicalLevelTechnique::RLE) ? rleMetadata.getRuns() : 0;

        if (logicalLevelTechnique1 == LogicalLevelTechnique::RLE) {
            assert(metadataType == LogicalLevelTechnique::RLE);
            return (rleRuns == 1) ? VectorType::CONST : VectorType::FLAT;
        } else if (logicalLevelTechnique1 == LogicalLevelTechnique::DELTA &&
                   logicalLevelTechnique2 == LogicalLevelTechnique::RLE) {
            assert(metadataType == LogicalLevelTechnique::RLE);
            // If base value equals delta value then one run else two runs
            if (rleRuns == 1 || rleRuns == 2) {
                return VectorType::SEQUENCE;
            }
        }

        return (streamMetadata.getNumValues() == 1) ? VectorType::CONST : VectorType::FLAT;
    }

public:
    std::unique_ptr<GeometryVector> decodeGeometryColumn(BufferStream& tileData,
                                                         const metadata::tileset::Column& column,
                                                         std::uint32_t numStreams) {
        using namespace util::decoding;
        using namespace metadata::stream;
        using namespace metadata::tileset;

        std::vector<GeometryType> geometryTypes;
        std::vector<std::uint32_t> geometryOffsets;
        std::vector<std::uint32_t> partOffsets;
        std::vector<std::uint32_t> ringOffsets;
        std::vector<std::uint32_t> vertexOffsets;
        std::vector<std::uint32_t> indexBuffer;
        std::vector<std::uint32_t> triangles;
        std::vector<std::int32_t> vertices;

        const auto geomTypeMetadata = StreamMetadata::decode(tileData);
        if (!geomTypeMetadata) {
            throw std::runtime_error("geometry column missing metadata stream: " + column.name);
        }

        [[maybe_unused]] const auto geometryTypesVectorType = getVectorTypeIntStream(*geomTypeMetadata);

        // If all geometries in the column have the same geometry type, we could decode them
        // somewhat more efficiently, and return the geometry in a more GPU-friendly form.
        // if (geometryTypesVectorType == VectorType::CONST) {
        //     const auto geomType = intDecoder.decodeConstIntStream<std::uint32_t, std::uint32_t, GeometryType>(
        //         tileData, *geomTypeMetadata);
        //     ...
        // }

        // Different geometry types are mixed in the geometry column
        intDecoder.decodeIntStream<std::uint32_t, std::uint32_t, GeometryType>(
            tileData, geometryTypes, *geomTypeMetadata);

        std::optional<geometry::MortonSettings> mortonSettings;

        for (std::uint32_t i = 1; i < numStreams; ++i) {
            const auto geomStreamMetadata = StreamMetadata::decode(tileData);

            switch (geomStreamMetadata->getPhysicalStreamType()) {
                case PhysicalStreamType::LENGTH: {
                    if (!geomStreamMetadata->getLogicalStreamType() ||
                        !geomStreamMetadata->getLogicalStreamType()->getLengthType()) {
                        throw std::runtime_error("Length stream missing logical type: " + column.name);
                    }
                    const auto type = *geomStreamMetadata->getLogicalStreamType()->getLengthType();
                    std::optional<std::reference_wrapper<std::vector<std::uint32_t>>> target;
                    switch (type) {
                        case LengthType::GEOMETRIES:
                            target = geometryOffsets;
                            break;
                        case LengthType::PARTS:
                            target = partOffsets;
                            break;
                        case LengthType::RINGS:
                            target = ringOffsets;
                            break;
                        case LengthType::TRIANGLES:
                            target = triangles;
                            break;
                        default:
                            throw std::runtime_error("Length stream type '" + std::to_string(std::to_underlying(type)) +
                                                     " not implemented: " + column.name);
                    }
                    intDecoder.decodeIntStream<std::uint32_t, std::uint32_t, std::uint32_t>(
                        tileData, target->get(), *geomStreamMetadata);
                    break;
                }
                case PhysicalStreamType::OFFSET: {
                    if (!geomStreamMetadata->getLogicalStreamType() ||
                        !geomStreamMetadata->getLogicalStreamType()->getOffsetType()) {
                        throw std::runtime_error("Offset stream missing type: " + column.name);
                    }
                    const auto type = *geomStreamMetadata->getLogicalStreamType()->getOffsetType();
                    std::optional<std::reference_wrapper<std::vector<std::uint32_t>>> target;
                    switch (type) {
                        case OffsetType::VERTEX:
                            target = vertexOffsets;
                            break;
                        case OffsetType::INDEX:
                            target = indexBuffer;
                            break;
                        default:
                            throw std::runtime_error("Offset stream type '" + std::to_string(std::to_underlying(type)) +
                                                     " not implemented: " + column.name);
                    }
                    intDecoder.decodeIntStream<std::uint32_t>(tileData, target->get(), *geomStreamMetadata);
                    break;
                }
                case PhysicalStreamType::DATA: {
                    if (!geomStreamMetadata->getLogicalStreamType() ||
                        !geomStreamMetadata->getLogicalStreamType()->getDictionaryType()) {
                        throw std::runtime_error("Data stream missing dictionary type: " + column.name);
                    }
                    if (mortonSettings) {
                        throw std::runtime_error("multiple data streams");
                    }
                    const auto type = *geomStreamMetadata->getLogicalStreamType()->getDictionaryType();
                    switch (type) {
                        case DictionaryType::VERTEX:
                            switch (geomStreamMetadata->getPhysicalLevelTechnique()) {
                                case PhysicalLevelTechnique::FAST_PFOR:
                                    throw std::runtime_error("FastPfor encoding for geometries is not yet supported.");
                                case PhysicalLevelTechnique::NONE:
                                case PhysicalLevelTechnique::ALP:
                                    // TODO: other implementations are not clear on whether these are valid
                                case PhysicalLevelTechnique::VARINT:
                                    intDecoder.decodeIntStream<std::uint32_t, std::uint32_t, std::int32_t>(
                                        tileData, vertices, *geomStreamMetadata, /*isSigned=*/true);
                                    break;
                                default:
                                    throw std::runtime_error("Unsupported encoding for geometries: " + column.name);
                            };
                            break;
                        case DictionaryType::MORTON: {
                            assert(geomStreamMetadata->getMetadataType() == LogicalLevelTechnique::MORTON);
                            const auto& mortonStreamMetadata = static_cast<const MortonEncodedStreamMetadata&>(
                                *geomStreamMetadata);
                            // TODO: This results in double-morton-decoding, need to align with TypeScript
                            // implementation. mortonSettings = geometry::MortonSettings{
                            //    .numBits = mortonStreamMetadata.getNumBits(),
                            //    .coordinateShift = mortonStreamMetadata.getCoordinateShift()};
                            intDecoder.decodeMortonStream<std::uint32_t, std::int32_t>(
                                tileData, vertices, mortonStreamMetadata);
                            break;
                        }
                        default:
                            throw std::runtime_error("Dictionary type '" + std::to_string(std::to_underlying(type)) +
                                                     "' not implemented: " + column.name);
                    }
                    break;
                }
                case PhysicalStreamType::PRESENT:
                    break;
                default:
                    throw std::runtime_error("Unsupported logical stream type: " + column.name);
            }
        }

        if (!indexBuffer.empty() && partOffsets.empty()) {
            /* Case when the indices of a Polygon outline are not encoded in the data so no
             *  topology data are present in the tile */
            return std::make_unique<geometry::FlatGpuVector>(
                std::move(geometryTypes), std::move(triangles), std::move(indexBuffer), std::move(vertices));
        }

        if (!geometryOffsets.empty()) {
            auto geometryOffsetsCopy = geometryOffsets; // TODO: avoid copies
            decodeRootLengthStream(geometryTypes,
                                   geometryOffsetsCopy,
                                   /*bufferId=*/GeometryType::POLYGON,
                                   geometryOffsets);
            if (!partOffsets.empty()) {
                if (!ringOffsets.empty()) {
                    auto partOffsetsCopy = partOffsets;
                    decodeLevel1LengthStream(geometryTypes,
                                             geometryOffsets,
                                             partOffsetsCopy,
                                             /*isLineStringPresent=*/false,
                                             partOffsets);
                    auto ringOffsetsCopy = ringOffsets;
                    decodeLevel2LengthStream(geometryTypes, geometryOffsets, partOffsets, ringOffsetsCopy, ringOffsets);
                } else {
                    auto partOffsetsCopy = partOffsets;
                    decodeLevel1WithoutRingBufferLengthStream(
                        geometryTypes, geometryOffsets, partOffsetsCopy, partOffsets);
                }
            }
        } else if (!partOffsets.empty()) {
            auto partOffsetsCopy = partOffsets;
            if (!ringOffsets.empty()) {
                auto ringOffsetsCopy = ringOffsets;
                decodeRootLengthStream(geometryTypes, partOffsetsCopy, GeometryType::LINESTRING, partOffsets);
                decodeLevel1LengthStream(geometryTypes,
                                         partOffsets,
                                         ringOffsetsCopy,
                                         /*isLineStringPresent=*/true,
                                         ringOffsets);
            } else {
                decodeRootLengthStream(geometryTypes, partOffsetsCopy, GeometryType::POINT, partOffsets);
            }
        }

        if (!indexBuffer.empty()) {
            /* Case when the indices of a Polygon outline are encoded in the tile */
            return std::make_unique<geometry::FlatGpuVector>(
                std::move(geometryTypes),
                std::move(triangles),
                std::move(indexBuffer),
                std::move(vertices),
                geometry::TopologyVector(std::move(geometryOffsets), std::move(partOffsets), std::move(ringOffsets)));
        }

        return std::make_unique<geometry::FlatGeometryVector>(
            std::move(geometryTypes),
            geometry::TopologyVector(std::move(geometryOffsets), std::move(partOffsets), std::move(ringOffsets)),
            std::move(vertexOffsets),
            std::move(vertices),
            mortonSettings ? geometry::VertexBufferType::MORTON : geometry::VertexBufferType::VEC_2,
            mortonSettings);
    }

    /*
     * Handle the parsing of the different topology length buffers separate not generic to reduce the
     * branching and improve the performance
     */
    void decodeRootLengthStream(const std::vector<metadata::tileset::GeometryType>& geometryTypes,
                                const std::vector<std::uint32_t>& rootLengthStream,
                                const metadata::tileset::GeometryType bufferId,
                                std::vector<std::uint32_t>& rootBufferOffsets) {
        assert(&rootLengthStream != &rootBufferOffsets);
        rootBufferOffsets.resize(geometryTypes.size() + 1);
        std::uint32_t previousOffset = rootBufferOffsets[0] = 0;
        std::uint32_t rootLengthCounter = 0;
        for (std::size_t i = 0; i < geometryTypes.size(); ++i) {
            /* Test if the geometry has and entry in the root buffer
             * BufferId: 2 GeometryOffsets -> MultiPolygon, MultiLineString, MultiPoint
             * BufferId: 1 PartOffsets -> Polygon
             * BufferId: 0 PartOffsets, RingOffsets -> LineString
             * */
            previousOffset = rootBufferOffsets[i + 1] = previousOffset + ((geometryTypes[i] > bufferId)
                                                                              ? rootLengthStream[rootLengthCounter++]
                                                                              : 1);
        }
    }

    void decodeLevel1LengthStream(const std::vector<metadata::tileset::GeometryType>& geometryTypes,
                                  const std::vector<std::uint32_t>& rootOffsetBuffer,
                                  const std::vector<std::uint32_t>& level1LengthBuffer,
                                  const bool isLineStringPresent,
                                  std::vector<std::uint32_t>& level1BufferOffsets) {
        assert(&rootOffsetBuffer != &level1BufferOffsets);
        assert(&level1LengthBuffer != &level1BufferOffsets);
        using metadata::tileset::GeometryType;
        level1BufferOffsets.resize(rootOffsetBuffer[rootOffsetBuffer.size() - 1] + 1);
        std::uint32_t previousOffset = level1BufferOffsets[0] = 0;
        std::uint32_t level1BufferCounter = 1;
        std::uint32_t level1LengthBufferCounter = 0;
        for (std::size_t i = 0; i < geometryTypes.size(); ++i) {
            const auto geometryType = geometryTypes[i];
            const auto numGeometries = rootOffsetBuffer[i + 1] - rootOffsetBuffer[i];

            if (geometryType == GeometryType::MULTIPOLYGON || geometryType == GeometryType::POLYGON ||
                (isLineStringPresent &&
                 (geometryType == GeometryType::MULTILINESTRING || geometryType == GeometryType::LINESTRING))) {
                /* For MultiPolygon, Polygon and in some cases for MultiLineString and LineString
                 * a value in the level1LengthBuffer exists */
                for (std::uint32_t j = 0; j < numGeometries; ++j) {
                    previousOffset = level1BufferOffsets[level1BufferCounter++] =
                        previousOffset + level1LengthBuffer[level1LengthBufferCounter++];
                }
            } else {
                /* For MultiPoint and Point and in some cases for MultiLineString and LineString no value in the
                 * level1LengthBuffer exists */
                for (std::uint32_t j = 0; j < numGeometries; j++) {
                    level1BufferOffsets[level1BufferCounter++] = ++previousOffset;
                }
            }
        }
    }

    /*
     * Case where no ring buffer exists so no MultiPolygon or Polygon geometry is part of the buffer
     */
    void decodeLevel1WithoutRingBufferLengthStream(const std::vector<metadata::tileset::GeometryType>& geometryTypes,
                                                   const std::vector<std::uint32_t>& rootOffsetBuffer,
                                                   const std::vector<std::uint32_t>& level1LengthBuffer,
                                                   std::vector<std::uint32_t>& level1BufferOffsets) {
        assert(&rootOffsetBuffer != &level1BufferOffsets);
        assert(&level1LengthBuffer != &level1BufferOffsets);
        using metadata::tileset::GeometryType;
        level1BufferOffsets.resize(rootOffsetBuffer[rootOffsetBuffer.size() - 1] + 1);
        std::uint32_t previousOffset = level1BufferOffsets[0] = 0;
        std::uint32_t level1OffsetBufferCounter = 1;
        std::uint32_t level1LengthCounter = 0;
        for (std::size_t i = 0; i < geometryTypes.size(); ++i) {
            const auto geometryType = geometryTypes[i];
            const auto numGeometries = rootOffsetBuffer[i + 1] - rootOffsetBuffer[i];
            if (geometryType == GeometryType::MULTILINESTRING || geometryType == GeometryType::LINESTRING) {
                /* For MultiLineString and LineString a value in the level1LengthBuffer exists */
                for (std::uint32_t j = 0; j < numGeometries; ++j) {
                    previousOffset = level1BufferOffsets[level1OffsetBufferCounter++] =
                        previousOffset + level1LengthBuffer[level1LengthCounter++];
                }
            } else {
                /* For MultiPoint and Point no value in level1LengthBuffer exists */
                for (std::uint32_t j = 0; j < numGeometries; ++j) {
                    level1BufferOffsets[level1OffsetBufferCounter++] = ++previousOffset;
                }
            }
        }
    }

    void decodeLevel2LengthStream(const std::vector<metadata::tileset::GeometryType>& geometryTypes,
                                  const std::vector<std::uint32_t>& rootOffsetBuffer,
                                  const std::vector<std::uint32_t>& level1OffsetBuffer,
                                  const std::vector<std::uint32_t>& level2LengthBuffer,
                                  std::vector<std::uint32_t>& level2BufferOffsets) {
        assert(&rootOffsetBuffer != &level2BufferOffsets);
        assert(&level1OffsetBuffer != &level2BufferOffsets);
        assert(&level2LengthBuffer != &level2BufferOffsets);
        using metadata::tileset::GeometryType;
        level2BufferOffsets.resize(level1OffsetBuffer[level1OffsetBuffer.size() - 1] + 1);
        std::uint32_t previousOffset = level2BufferOffsets[0] = 0;
        std::uint32_t level1OffsetBufferCounter = 1;
        std::uint32_t level2OffsetBufferCounter = 1;
        std::uint32_t level2LengthBufferCounter = 0;
        for (std::size_t i = 0; i < geometryTypes.size(); ++i) {
            const auto geometryType = geometryTypes[i];
            const auto numGeometries = rootOffsetBuffer[i + 1] - rootOffsetBuffer[i];
            if (geometryType != GeometryType::POINT && geometryType != GeometryType::MULTIPOINT) {
                /* For MultiPolygon, MultiLineString, Polygon and LineString a value in level2LengthBuffer
                 * exists */
                for (std::uint32_t j = 0; j < numGeometries; ++j) {
                    const auto numParts = level1OffsetBuffer[level1OffsetBufferCounter] -
                                          level1OffsetBuffer[level1OffsetBufferCounter - 1];
                    level1OffsetBufferCounter++;
                    for (std::uint32_t k = 0; k < numParts; ++k) {
                        previousOffset = level2BufferOffsets[level2OffsetBufferCounter++] =
                            previousOffset + level2LengthBuffer[level2LengthBufferCounter++];
                    }
                }
            } else {
                /* For MultiPoint and Point no value in level2LengthBuffer exists */
                for (std::uint32_t j = 0; j < numGeometries; j++) {
                    level2BufferOffsets[level2OffsetBufferCounter++] = ++previousOffset;
                    level1OffsetBufferCounter++;
                }
            }
        }
    }

private:
    IntegerDecoder intDecoder;
};

} // namespace mlt::decoder
