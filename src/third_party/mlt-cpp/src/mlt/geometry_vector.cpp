#include <mlt/geometry_vector.hpp>

#include <mlt/coordinate.hpp>
#include <mlt/polyfill.hpp>
#include <mlt/util/morton_curve.hpp>
#include <mlt/util/stl.hpp>

namespace mlt::geometry {

namespace {
constexpr void checkBuffer(std::size_t index, std::size_t size, std::string_view name) {
    if (index >= size) {
        throw std::runtime_error(std::string(name) + " underflow");
    }
}
#define CHECK_BUFFER(I, B) checkBuffer(I, B.size(), #B)

inline Coordinate coord(std::int32_t x, std::int32_t y) {
    return {static_cast<float>(x), static_cast<float>(y)};
}

std::vector<Coordinate> getLineStringCoords(const std::vector<std::int32_t>& vertexBuffer,
                                            std::uint32_t startIndex,
                                            const std::uint32_t numVertices,
                                            bool closeLineString) {
    std::vector<Coordinate> coords;
    coords.reserve(numVertices + 1);
    CHECK_BUFFER(startIndex + numVertices - 1, vertexBuffer);
    for (std::uint32_t i = 0; i < numVertices; ++i) {
        const auto x = vertexBuffer[startIndex++];
        coords.push_back(coord(x, vertexBuffer[startIndex++]));
    }

    if (closeLineString && !coords.empty() && coords.front() != coords.back()) {
        coords.push_back(coords.front());
    }
    return coords;
}

std::vector<Coordinate> getDictionaryEncodedLineStringCoords(const std::vector<std::int32_t>& vertexBuffer,
                                                             const std::vector<std::uint32_t>& vertexOffsets,
                                                             std::uint32_t vertexOffset,
                                                             const std::uint32_t numVertices,
                                                             const bool closeLineString) {
    std::vector<Coordinate> coords;
    coords.reserve(numVertices + 1);
    CHECK_BUFFER(vertexOffset + numVertices - 1, vertexOffsets);
    for (std::uint32_t i = 0; i < numVertices; ++i) {
        const auto offset = 2 * vertexOffsets[vertexOffset++];
        CHECK_BUFFER(offset + 1, vertexBuffer);
        coords.push_back(coord(vertexBuffer[offset], vertexBuffer[offset + 1]));
    }
    if (closeLineString && !coords.empty() && coords.front() != coords.back()) {
        coords.push_back(coords.front());
    }
    return coords;
}

std::vector<Coordinate> getMortonEncodedLineStringCoords(const std::vector<std::int32_t>& vertexBuffer,
                                                         const std::vector<std::uint32_t>& vertexOffsets,
                                                         std::uint32_t vertexOffset,
                                                         const std::uint32_t numVertices,
                                                         const MortonSettings& mortonSettings,
                                                         const bool closeLineString) {
    std::vector<Coordinate> coords;
    coords.reserve(numVertices + 1);
    CHECK_BUFFER(vertexOffset + numVertices - 1, vertexOffsets);
    for (std::uint32_t i = 0; i < numVertices; ++i) {
        const auto offset = vertexOffsets[vertexOffset++];
        CHECK_BUFFER(offset, vertexBuffer);
        coords.push_back(
            util::MortonCurve::decode(vertexBuffer[offset], mortonSettings.numBits, mortonSettings.coordinateShift));
    }
    if (closeLineString && !coords.empty() && coords.front() != coords.back()) {
        coords.push_back(coords.front());
    }
    return coords;
}

} // namespace

void GeometryVector::applyTriangles(Geometry& geom,
                                    std::uint32_t& triangleOffset,
                                    std::uint32_t& indexBufferOffset,
                                    [[maybe_unused]] std::uint32_t totalVertices,
                                    [[maybe_unused]] bool multiPolygon) const {
    if (triangleCounts.empty()) {
        return;
    }

    CHECK_BUFFER(triangleOffset, triangleCounts);
    const auto numTriangles = triangleCounts[triangleOffset++];
    if (numTriangles) {
        CHECK_BUFFER(indexBufferOffset + (3 * numTriangles) - 1, indexBuffer);
        assert(std::all_of(std::next(indexBuffer.cbegin(), indexBufferOffset),
                           std::next(indexBuffer.cbegin(), indexBufferOffset + (3 * numTriangles)),
                           [=](auto i) { return i < totalVertices; }));
#if !defined(NDEBUG) && false
        // Expect the tessellated indexes to reference the entire range of vertices.
        // The Java implementation of Earcut makes this fail, so it's disabled for now.
        const auto limits = std::ranges::minmax_element(&indexBuffer[indexBufferOffset],
                                                        &indexBuffer[indexBufferOffset + (3 * numTriangles)]);
        assert(*limits.min == 0 && *limits.max == totalVertices - 1);
#endif
        geom.setTriangles({&indexBuffer[indexBufferOffset], 3 * numTriangles});
        indexBufferOffset += 3 * numTriangles;
    }
}

std::vector<std::unique_ptr<Geometry>> GeometryVector::getGeometries(const GeometryFactory& factory) const {
    std::vector<std::unique_ptr<Geometry>> geometries;
    geometries.reserve(numGeometries);

    std::uint32_t partOffsetCounter = 1;
    std::uint32_t ringOffsetsCounter = 1;
    std::uint32_t geometryOffsetsCounter = 1;
    std::uint32_t vertexBufferOffset = 0;
    std::uint32_t vertexOffsetsOffset = 0;
    std::uint32_t indexBufferOffset = 0;
    std::uint32_t triangleOffset = 0;

    const auto containsPolygon = containsPolygonGeometry();

    for (std::size_t i = 0; i < numGeometries; ++i) {
        using metadata::tileset::GeometryType;
        const auto geomType = getGeometryType(i);

        switch (geomType) {
            case GeometryType::POINT:
                if (vertexOffsets.empty()) {
                    CHECK_BUFFER(vertexBufferOffset + 1, vertexBuffer);
                    geometries.push_back(
                        factory.createPoint({static_cast<float>(vertexBuffer[vertexBufferOffset]),
                                             static_cast<float>(vertexBuffer[vertexBufferOffset + 1])}));
                    vertexBufferOffset += 2;
                } else if (vertexBufferType == VertexBufferType::VEC_2) {
                    CHECK_BUFFER(vertexOffsetsOffset, vertexOffsets);
                    const auto vertexOffset = vertexOffsets[vertexOffsetsOffset++] * 2;
                    CHECK_BUFFER(vertexOffset + 1, vertexBuffer);
                    geometries.push_back(factory.createPoint({static_cast<float>(vertexBuffer[vertexOffset]),
                                                              static_cast<float>(vertexBuffer[vertexOffset + 1])}));
                } else {
                    assert(vertexBufferType == VertexBufferType::MORTON);
                    CHECK_BUFFER(vertexOffsetsOffset, vertexOffsets);
                    const auto mortonCode = vertexOffsets[vertexOffsetsOffset++];
                    geometries.push_back(factory.createPoint(util::MortonCurve::decode(
                        mortonCode, mortonSettings->numBits, mortonSettings->coordinateShift)));
                }

                geometryOffsetsCounter++;
                partOffsetCounter++;
                ringOffsetsCounter++;

                break;
            case GeometryType::MULTIPOINT: {
                if (!topologyVector) {
                    throw std::runtime_error("Multi-point geometry without topology vector");
                }
                const auto& geometryOffsets = topologyVector->getGeometryOffsets();
                [[maybe_unused]] const auto& partOffsets = topologyVector->getPartOffsets();
                [[maybe_unused]] const auto& ringOffsets = topologyVector->getRingOffsets();

                CHECK_BUFFER(geometryOffsetsCounter, geometryOffsets);
                const auto numPoints = geometryOffsets[geometryOffsetsCounter] -
                                       geometryOffsets[geometryOffsetsCounter - 1];
                geometryOffsetsCounter++;

                if (numPoints) {
                    std::vector<Coordinate> vertices;
                    vertices.reserve(numPoints);

                    if (vertexOffsets.empty()) {
                        CHECK_BUFFER(vertexBufferOffset + (2 * numPoints) - 1, vertexBuffer);
                        for (std::uint32_t j = 0; j < numPoints; ++j) {
                            const auto x = vertexBuffer[vertexBufferOffset++];
                            vertices.push_back(coord(x, vertexBuffer[vertexBufferOffset++]));
                        }
                    } else {
                        CHECK_BUFFER(vertexOffsetsOffset + numPoints - 1, vertexOffsets);
                        for (std::uint32_t j = 0; j < numPoints; ++j) {
                            const auto offset = vertexOffsets[vertexOffsetsOffset++] * 2;
                            vertices.push_back(coord(vertexBuffer[offset], vertexBuffer[offset + 1]));
                        }
                    }
                    geometries.push_back(factory.createMultiPoint(std::move(vertices)));
                }
                break;
            }
            case GeometryType::LINESTRING: {
                if (!topologyVector) {
                    throw std::runtime_error("Linestring geometry without topology vector");
                }

                std::uint32_t numVertices = 0;
                if (containsPolygon) {
                    const auto& ringOffsets = topologyVector->getRingOffsets();
                    CHECK_BUFFER(ringOffsetsCounter, ringOffsets);
                    numVertices = ringOffsets[ringOffsetsCounter] - ringOffsets[ringOffsetsCounter - 1];
                    ringOffsetsCounter++;
                } else {
                    const auto& partOffsets = topologyVector->getPartOffsets();
                    CHECK_BUFFER(partOffsetCounter, partOffsets);
                    numVertices = partOffsets[partOffsetCounter] - partOffsets[partOffsetCounter - 1];
                }
                partOffsetCounter++;

                std::vector<Coordinate> vertices;
                if (vertexOffsets.empty()) {
                    vertices = getLineStringCoords(
                        vertexBuffer, vertexBufferOffset, numVertices, /*closeLineString=*/false);
                    vertexBufferOffset += numVertices * 2;
                } else {
                    vertices = (vertexBufferType == VertexBufferType::VEC_2)
                                   ? getDictionaryEncodedLineStringCoords(vertexBuffer,
                                                                          vertexOffsets,
                                                                          vertexOffsetsOffset,
                                                                          numVertices,
                                                                          /*closeLineString=*/false)
                                   : getMortonEncodedLineStringCoords(vertexBuffer,
                                                                      vertexOffsets,
                                                                      vertexOffsetsOffset,
                                                                      numVertices,
                                                                      *mortonSettings,
                                                                      /*closeLineString=*/false);
                    vertexOffsetsOffset += numVertices;
                }

                geometryOffsetsCounter++;

                geometries.push_back(factory.createLineString(std::move(vertices)));
                break;
            }
            case GeometryType::POLYGON: {
                if (!topologyVector) {
                    throw std::runtime_error("Polygon geometry without topology vector");
                }
                [[maybe_unused]] const auto& geometryOffsets = topologyVector->getGeometryOffsets();
                const auto& partOffsets = topologyVector->getPartOffsets();
                const auto& ringOffsets = topologyVector->getRingOffsets();

                CHECK_BUFFER(partOffsetCounter, partOffsets);
                const auto numRings = partOffsets[partOffsetCounter] - partOffsets[partOffsetCounter - 1];
                partOffsetCounter++;

                CHECK_BUFFER(ringOffsetsCounter, ringOffsets);
                const auto numVertices = ringOffsets[ringOffsetsCounter] - ringOffsets[ringOffsetsCounter - 1];
                ringOffsetsCounter++;

                std::vector<CoordVec> rings;
                rings.reserve(numRings);

                auto totalVertices = numVertices;
                if (vertexOffsets.empty()) {
                    rings.push_back(
                        getLineStringCoords(vertexBuffer, vertexBufferOffset, numVertices, /*closeLineString=*/true));
                    vertexBufferOffset += numVertices * 2;
                    assert(triangleCounts.empty() || numVertices == rings.back().size() ||
                           numVertices == rings.back().size() - 1);

                    for (std::uint32_t j = 1; j < numRings; ++j) {
                        CHECK_BUFFER(ringOffsetsCounter, ringOffsets);
                        const auto numRingVertices = ringOffsets[ringOffsetsCounter] -
                                                     ringOffsets[ringOffsetsCounter - 1];
                        totalVertices += numRingVertices;
                        ringOffsetsCounter++;
                        const auto vbo = vertexBufferOffset;
                        vertexBufferOffset += numRingVertices * 2;
                        rings.push_back(
                            getLineStringCoords(vertexBuffer, vbo, numRingVertices, /*closeLineString=*/true));
                        assert(triangleCounts.empty() || numRingVertices == rings.back().size() ||
                               numRingVertices == rings.back().size() - 1);
                    }

                    auto newGeometry = factory.createPolygon(std::move(rings));
                    applyTriangles(*newGeometry, triangleOffset, indexBufferOffset, totalVertices, false);
                    geometries.push_back(std::move(newGeometry));
                } else {
                    rings.push_back((vertexBufferType == VertexBufferType::VEC_2)
                                        ? getDictionaryEncodedLineStringCoords(vertexBuffer,
                                                                               vertexOffsets,
                                                                               vertexOffsetsOffset,
                                                                               numVertices,
                                                                               /*closeLineString=*/true)
                                        : getMortonEncodedLineStringCoords(vertexBuffer,
                                                                           vertexOffsets,
                                                                           vertexOffsetsOffset,
                                                                           numVertices,
                                                                           *mortonSettings,
                                                                           /*closeLineString=*/true));
                    vertexOffsetsOffset += numVertices;
                    assert(triangleCounts.empty() || numVertices == rings.back().size() ||
                           numVertices == rings.back().size() - 1);

                    auto dictTotalVertices = numVertices;
                    for (std::uint32_t j = 1; j < numRings; ++j) {
                        CHECK_BUFFER(ringOffsetsCounter, ringOffsets);
                        const auto numRingVertices = ringOffsets[ringOffsetsCounter] -
                                                     ringOffsets[ringOffsetsCounter - 1];
                        ringOffsetsCounter++;

                        dictTotalVertices += numRingVertices;

                        const auto offset = vertexOffsetsOffset;
                        vertexOffsetsOffset += numRingVertices;

                        rings.push_back(
                            (vertexBufferType == VertexBufferType::VEC_2)
                                ? getDictionaryEncodedLineStringCoords(
                                      vertexBuffer, vertexOffsets, offset, numRingVertices, /*closeLineString=*/true)
                                : getMortonEncodedLineStringCoords(vertexBuffer,
                                                                   vertexOffsets,
                                                                   offset,
                                                                   numRingVertices,
                                                                   *mortonSettings,
                                                                   /*closeLineString=*/true));
                        assert(triangleCounts.empty() || numRingVertices == rings.back().size());
                    }

                    auto newGeometry = factory.createPolygon(std::move(rings));
                    applyTriangles(*newGeometry, triangleOffset, indexBufferOffset, dictTotalVertices, false);
                    geometries.push_back(std::move(newGeometry));
                }

                if (topologyVector && !topologyVector->getGeometryOffsets().empty()) {
                    geometryOffsetsCounter++;
                }
                break;
            }
            case GeometryType::MULTILINESTRING: {
                if (!topologyVector) {
                    throw std::runtime_error("Multi-Linestring geometry without topology vector");
                }
                const auto& geometryOffsets = topologyVector->getGeometryOffsets();
                const auto& partOffsets = topologyVector->getPartOffsets();
                const auto& ringOffsets = topologyVector->getRingOffsets();

                CHECK_BUFFER(geometryOffsetsCounter, geometryOffsets);
                const auto numLineStrings = geometryOffsets[geometryOffsetsCounter] -
                                            geometryOffsets[geometryOffsetsCounter - 1];
                geometryOffsetsCounter++;

                std::vector<CoordVec> lineStrings;
                lineStrings.reserve(numLineStrings);

                if (vertexOffsets.empty()) {
                    for (std::uint32_t j = 0; j < numLineStrings; j++) {
                        std::uint32_t numVertices = 0;
                        if (containsPolygon) {
                            CHECK_BUFFER(ringOffsetsCounter, ringOffsets);
                            numVertices = ringOffsets[ringOffsetsCounter] - ringOffsets[ringOffsetsCounter - 1];
                            ringOffsetsCounter++;
                        } else {
                            CHECK_BUFFER(partOffsetCounter, partOffsets);
                            numVertices = partOffsets[partOffsetCounter] - partOffsets[partOffsetCounter - 1];
                        }
                        partOffsetCounter++;

                        lineStrings.push_back(getLineStringCoords(
                            vertexBuffer, vertexBufferOffset, numVertices, /*closeLineString=*/false));
                        vertexBufferOffset += numVertices * 2;
                    }
                    geometries.push_back(factory.createMultiLineString(std::move(lineStrings)));
                } else {
                    for (std::uint32_t j = 0; j < numLineStrings; ++j) {
                        std::uint32_t numVertices = 0;
                        if (containsPolygon) {
                            CHECK_BUFFER(ringOffsetsCounter, ringOffsets);
                            numVertices = ringOffsets[ringOffsetsCounter] - ringOffsets[ringOffsetsCounter - 1];
                            ringOffsetsCounter++;
                        } else {
                            CHECK_BUFFER(partOffsetCounter, partOffsets);
                            numVertices = partOffsets[partOffsetCounter] - partOffsets[partOffsetCounter - 1];
                        }
                        partOffsetCounter++;

                        lineStrings.push_back((vertexBufferType == VertexBufferType::VEC_2)
                                                  ? getDictionaryEncodedLineStringCoords(vertexBuffer,
                                                                                         vertexOffsets,
                                                                                         vertexOffsetsOffset,
                                                                                         numVertices,
                                                                                         /*closeLineString=*/false)
                                                  : getMortonEncodedLineStringCoords(vertexBuffer,
                                                                                     vertexOffsets,
                                                                                     vertexOffsetsOffset,
                                                                                     numVertices,
                                                                                     *mortonSettings,
                                                                                     /*closeLineString=*/false));
                        vertexOffsetsOffset += numVertices;
                    }
                    geometries.push_back(factory.createMultiLineString(std::move(lineStrings)));
                }
                break;
            }
            case GeometryType::MULTIPOLYGON: {
                if (!topologyVector) {
                    throw std::runtime_error("Multi-polygon geometry without topology vector");
                }
                const auto& geometryOffsets = topologyVector->getGeometryOffsets();
                const auto& partOffsets = topologyVector->getPartOffsets();
                const auto& ringOffsets = topologyVector->getRingOffsets();

                CHECK_BUFFER(geometryOffsetsCounter, geometryOffsets);
                const auto numPolygons = geometryOffsets[geometryOffsetsCounter] -
                                         geometryOffsets[geometryOffsetsCounter - 1];
                geometryOffsetsCounter++;

                std::vector<MultiPolygon::RingVec> polygons;
                polygons.reserve(numPolygons);

                if (vertexOffsets.empty()) {
                    [[maybe_unused]] std::uint32_t plainTotalVertices = 0;
                    for (std::uint32_t j = 0; j < numPolygons; ++j) {
                        CHECK_BUFFER(partOffsetCounter, partOffsets);
                        const auto numRings = partOffsets[partOffsetCounter] - partOffsets[partOffsetCounter - 1];
                        partOffsetCounter++;

                        CHECK_BUFFER(ringOffsetsCounter, ringOffsets);
                        const auto numVertices = ringOffsets[ringOffsetsCounter] - ringOffsets[ringOffsetsCounter - 1];
                        ringOffsetsCounter++;

                        plainTotalVertices += numVertices;

                        std::vector<CoordVec> rings;
                        rings.reserve(numRings);

                        rings.push_back(getLineStringCoords(
                            vertexBuffer, vertexBufferOffset, numVertices, /*closeLineString=*/true));
                        vertexBufferOffset += numVertices * 2;

                        for (std::uint32_t k = 1; k < numRings; ++k) {
                            CHECK_BUFFER(ringOffsetsCounter, ringOffsets);
                            const auto numRingVertices = ringOffsets[ringOffsetsCounter] -
                                                         ringOffsets[ringOffsetsCounter - 1];
                            ringOffsetsCounter++;

                            plainTotalVertices += numRingVertices;

                            rings.push_back(getLineStringCoords(
                                vertexBuffer, vertexBufferOffset, numRingVertices, /*closeLineString=*/true));
                            vertexBufferOffset += numRingVertices * 2;
                        }

                        polygons.push_back(std::move(rings));
                    }

                    auto newGeometry = factory.createMultiPolygon(std::move(polygons));
                    applyTriangles(*newGeometry, triangleOffset, indexBufferOffset, plainTotalVertices, true);
                    geometries.push_back(std::move(newGeometry));
                } else {
                    [[maybe_unused]] std::uint32_t dictTotalVertices = 0;
                    for (std::uint32_t j = 0; j < numPolygons; ++j) {
                        CHECK_BUFFER(partOffsetCounter, partOffsets);
                        const auto numRings = partOffsets[partOffsetCounter] - partOffsets[partOffsetCounter - 1];
                        partOffsetCounter++;

                        CHECK_BUFFER(ringOffsetsCounter, ringOffsets);
                        const auto numVertices = ringOffsets[ringOffsetsCounter] - ringOffsets[ringOffsetsCounter - 1];
                        ringOffsetsCounter++;

                        dictTotalVertices += numVertices;

                        std::vector<CoordVec> rings;
                        rings.reserve(numRings);

                        rings.push_back((vertexBufferType == VertexBufferType::VEC_2)
                                            ? getDictionaryEncodedLineStringCoords(vertexBuffer,
                                                                                   vertexOffsets,
                                                                                   vertexOffsetsOffset,
                                                                                   numVertices,
                                                                                   /*closeLineString=*/true)
                                            : getMortonEncodedLineStringCoords(vertexBuffer,
                                                                               vertexOffsets,
                                                                               vertexOffsetsOffset,
                                                                               numVertices,
                                                                               *mortonSettings,
                                                                               /*closeLineString=*/true));
                        vertexOffsetsOffset += numVertices;

                        for (std::uint32_t k = 1; k < numRings; ++k) {
                            CHECK_BUFFER(ringOffsetsCounter, ringOffsets);
                            const auto numRingVertices = ringOffsets[ringOffsetsCounter] -
                                                         ringOffsets[ringOffsetsCounter - 1];
                            ringOffsetsCounter++;

                            dictTotalVertices += numRingVertices;

                            rings.push_back((vertexBufferType == VertexBufferType::VEC_2)
                                                ? getDictionaryEncodedLineStringCoords(vertexBuffer,
                                                                                       vertexOffsets,
                                                                                       vertexOffsetsOffset,
                                                                                       numRingVertices,
                                                                                       /*closeLineString=*/true)
                                                : getMortonEncodedLineStringCoords(vertexBuffer,
                                                                                   vertexOffsets,
                                                                                   vertexOffsetsOffset,
                                                                                   numRingVertices,
                                                                                   *mortonSettings,
                                                                                   /*closeLineString=*/true));
                            vertexOffsetsOffset += numRingVertices;
                        }

                        polygons.push_back(std::move(rings));
                    }

                    auto newGeometry = factory.createMultiPolygon(std::move(polygons));
                    applyTriangles(*newGeometry, triangleOffset, indexBufferOffset, dictTotalVertices, true);
                    geometries.push_back(std::move(newGeometry));
                }
                break;
            }
            default:
                throw std::runtime_error("Unsupported geometry type: " + std::to_string(std::to_underlying(geomType)));
        }
    }

    // If we didn't use all the input data, that's a bug.
    assert(indexBufferOffset == indexBuffer.size());
    assert(triangleOffset == triangleCounts.size());

    // TODO: this fails for `test/expected/amazon_here/8_132_85.mlt`
    // assert(!topologyVector || topologyVector->getPartOffsets().empty() ||
    //        partOffsetCounter == topologyVector->getPartOffsets().size());
    assert(!topologyVector || topologyVector->getRingOffsets().empty() ||
           ringOffsetsCounter == topologyVector->getRingOffsets().size());
    assert(!topologyVector || topologyVector->getGeometryOffsets().empty() ||
           geometryOffsetsCounter == topologyVector->getGeometryOffsets().size());

    // If we're using vertex offsets, we should have used all
    // of them, and we won't have used vertex buffer offsets.
    assert(!vertexOffsets.empty() || vertexBufferOffset == vertexBuffer.size());
    assert(vertexOffsets.empty() || vertexOffsetsOffset == vertexOffsets.size());

    return geometries;
}

} // namespace mlt::geometry
