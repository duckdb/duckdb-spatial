#include <mlt/encoder.hpp>

#include <mlt/encode/boolean.hpp>
#include <mlt/encode/float.hpp>
#include <mlt/encode/geometry.hpp>
#include <mlt/encode/int.hpp>
#include <mlt/encode/property.hpp>
#include <mlt/encode/string.hpp>
#include <mlt/metadata/stream.hpp>
#include <mlt/metadata/tileset.hpp>
#include <mlt/util/encoding/varint.hpp>
#include <mlt/util/hilbert_curve.hpp>
#include <mlt/util/stl.hpp>
#include <mlt/util/string.hpp>

#include <mapbox/earcut.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace mlt {

using encoder::BooleanEncoder;
using encoder::FloatEncoder;
using encoder::GeometryEncoder;
using encoder::IntegerEncoder;
using encoder::PropertyEncoder;
using encoder::StringEncoder;

using metadata::tileset::Column;
using metadata::tileset::ColumnScope;
using metadata::tileset::ComplexColumn;
using metadata::tileset::ComplexType;
using metadata::tileset::FeatureTable;
using metadata::tileset::GeometryType;
using metadata::tileset::LogicalScalarType;
using metadata::tileset::ScalarColumn;
using metadata::tileset::ScalarType;

using metadata::stream::PhysicalLevelTechnique;

namespace {
// `std::visit` requires exhaustive overloads, but the type is determined
// by column metadata, so the catch-all arms are dead by construction.
// This is used in cases that should be impossible to reach.
// GCOVR_EXCL_START
void throwInvalidType() {
    throw std::runtime_error("Invalid type");
};
// GCOVR_EXCL_STOP
} // namespace

struct Encoder::Impl {
    using EncodedChunks = std::vector<std::vector<std::uint8_t>>;
    using PropertyValuePtr = const Encoder::PropertyValue*;
    using ScalarPropertyCache = std::unordered_map<std::string, std::vector<PropertyValuePtr>>;

    struct IdStats {
        bool hasLongId = false;
        bool hasMissingId = false;
    };

    IntegerEncoder intEncoder;

    static bool structValueHasChild(const Encoder::StructValue& sv,
                                    const std::string& rootName,
                                    const std::string& childName);
    static const std::string* resolveStructSourceKey(const std::vector<Feature>& features,
                                                     const metadata::tileset::Column& column);

    std::vector<std::uint8_t> encodeLayer(const Layer& layer, const EncoderConfig& config);

    static IdStats collectIdStats(const std::vector<Feature>& features);
    static FeatureTable buildMetadata(const Layer& layer, const EncoderConfig& config, std::optional<IdStats> idStats);

    static void collectGeometry(const std::vector<Feature>& features,
                                std::vector<GeometryType>& geometryTypes,
                                std::vector<std::uint32_t>& numGeometries,
                                std::vector<std::uint32_t>& numParts,
                                std::vector<std::uint32_t>& numRings,
                                std::vector<GeometryEncoder::Vertex>& vertexBuffer);

    static std::vector<Encoder::Feature> sortFeatures(const std::vector<Encoder::Feature>& features);

    static bool allPolygons(const std::vector<Encoder::Feature>& features);
    static void tessellateFeatures(const std::vector<Feature>& features,
                                   std::vector<std::uint32_t>& numTriangles,
                                   std::vector<std::uint32_t>& indexBuffer);
    static ScalarPropertyCache buildScalarPropertyCache(const FeatureTable& featureTable,
                                                        const std::vector<Feature>& features);
    static std::vector<std::uint8_t> assembleLayerBytes(const std::vector<std::uint8_t>& metadataBytes,
                                                        const EncodedChunks& bodyChunks);
};

bool Encoder::Impl::structValueHasChild(const Encoder::StructValue& sv,
                                        const std::string& rootName,
                                        const std::string& childName) {
    if (sv.contains(childName)) {
        return true;
    }
    return sv.contains(rootName + childName);
}

const std::string* Encoder::Impl::resolveStructSourceKey(const std::vector<Feature>& features,
                                                         const metadata::tileset::Column& column) {
    if (!column.isStruct()) {
        return nullptr;
    }

    const auto& complex = column.getComplexType();
    const auto& rootName = column.name;

    const std::string* bestKey = nullptr;
    std::size_t bestScore = 0;

    for (const auto& f : features) {
        for (const auto& [propertyKey, propertyValue] : f.properties) {
            const auto* sv = std::get_if<Encoder::StructValue>(&propertyValue);
            if (!sv) {
                continue;
            }

            std::size_t score = 0;
            for (const auto& child : complex.children) {
                if (structValueHasChild(*sv, rootName, child.name)) {
                    ++score;
                }
            }

            if (score > bestScore) {
                bestScore = score;
                bestKey = &propertyKey;
                if (score == complex.children.size()) {
                    return bestKey;
                }
            }
        }
    }

    return bestKey;
}

Encoder::Impl::IdStats Encoder::Impl::collectIdStats(const std::vector<Feature>& features) {
    IdStats idStats;
    for (const auto& feature : features) {
        if (!feature.id.has_value()) {
            idStats.hasMissingId = true;
            continue;
        }
        // Use 64-bit when any ID exceeds INT32_MAX: delta encoding accumulates in
        // int32_t, so uint32 values with bit 31 set would sign-extend on widening.
        if (*feature.id > std::numeric_limits<std::int32_t>::max()) {
            idStats.hasLongId = true;
        }
        if (idStats.hasLongId && idStats.hasMissingId) {
            break;
        }
    }
    return idStats;
}

FeatureTable Encoder::Impl::buildMetadata(const Layer& layer,
                                          const EncoderConfig& config,
                                          std::optional<IdStats> idStats) {
    FeatureTable table{.name = layer.name, .extent = layer.extent};

    if (config.includeIds) {
        const auto stats = idStats.value_or(collectIdStats(layer.features));

        table.columns.push_back(Column{
            .nullable = stats.hasMissingId,
            .columnScope = ColumnScope::FEATURE,
            .type = ScalarColumn{.type = LogicalScalarType::ID, .hasLongID = stats.hasLongId},
        });
    }

    table.columns.push_back(Column{
        .nullable = false,
        .columnScope = ColumnScope::FEATURE,
        .type = ComplexColumn{.type = ComplexType::GEOMETRY},
    });

    struct ColumnInfo {
        ScalarType type;
        std::size_t presentCount;
    };
    std::map<std::string, ColumnInfo> scalarColumns;
    std::map<std::string, std::set<std::string>> structColumnsBySourceKey;

    for (const auto& feature : layer.features) {
        for (const auto& [key, value] : feature.properties) {
            if (std::holds_alternative<Encoder::StructValue>(value)) {
                const auto& sv = std::get<Encoder::StructValue>(value);
                auto& children = structColumnsBySourceKey[key];
                for (const auto& [childName, _] : sv) {
                    children.insert(childName);
                }
                continue;
            }

            const auto scalarType = std::visit(
                util::overloaded{
                    [](bool) { return ScalarType::BOOLEAN; },
                    [](std::int32_t) { return ScalarType::INT_32; },
                    [](std::int64_t) { return ScalarType::INT_64; },
                    [](std::uint32_t) { return ScalarType::UINT_32; },
                    [](std::uint64_t) { return ScalarType::UINT_64; },
                    [](float) { return ScalarType::FLOAT; },
                    [](double) { return ScalarType::DOUBLE; },
                    [](const std::string&) { return ScalarType::STRING; },
                    [](const Encoder::StructValue&) -> ScalarType { throwInvalidType(); }, // GCOVR_EXCL_LINE
                },
                value);

            auto [it, inserted] = scalarColumns.try_emplace(key, ColumnInfo{.type = scalarType, .presentCount = 0});
            ++it->second.presentCount;
            if (!inserted) {
                auto& existing = it->second;
                if (existing.type != scalarType) {
                    if ((existing.type == ScalarType::INT_32 && scalarType == ScalarType::INT_64) ||
                        (existing.type == ScalarType::FLOAT && scalarType == ScalarType::DOUBLE)) {
                        existing.type = scalarType;
                    } else if ((existing.type == ScalarType::INT_64 && scalarType == ScalarType::INT_32) ||
                               (existing.type == ScalarType::DOUBLE && scalarType == ScalarType::FLOAT)) {
                        // Keep the wider existing type.
                    } else {
                        existing.type = ScalarType::STRING;
                    }
                }
            }
        }
    }

    for (const auto& [name, info] : scalarColumns) {
        table.columns.push_back(Column{
            .name = name,
            .nullable = config.forceNullableColumns || info.presentCount != layer.features.size(),
            .columnScope = ColumnScope::FEATURE,
            .type = ScalarColumn{.type = info.type},
        });
    }

    for (const auto& [sourceKey, childNames] : structColumnsBySourceKey) {
        const auto derivedRoot = util::longestCommonPrefix(childNames);
        const bool hasDerivedRoot = childNames.size() > 1 && !derivedRoot.empty();

        Column col;
        col.name = hasDerivedRoot ? derivedRoot : sourceKey;
        col.nullable = false;
        col.columnScope = ColumnScope::FEATURE;

        ComplexColumn complex;
        complex.type = ComplexType::STRUCT;
        for (const auto& childName : childNames) {
            complex.children.push_back({
                .name = hasDerivedRoot ? childName.substr(derivedRoot.size()) : childName,
                .nullable = true,
                .columnScope = ColumnScope::FEATURE,
                .type = ScalarColumn{.type = ScalarType::STRING},
            });
        }
        col.type = std::move(complex);
        table.columns.push_back(std::move(col));
    }

    return table;
}

void Encoder::Impl::collectGeometry(const std::vector<Feature>& features,
                                    std::vector<metadata::tileset::GeometryType>& geometryTypes,
                                    std::vector<std::uint32_t>& numGeometries,
                                    std::vector<std::uint32_t>& numParts,
                                    std::vector<std::uint32_t>& numRings,
                                    std::vector<GeometryEncoder::Vertex>& vertexBuffer) {
    const bool containsPolygon = std::ranges::any_of(features, [](const auto& f) {
        return f.geometry.type == GeometryType::POLYGON || f.geometry.type == GeometryType::MULTIPOLYGON;
    });

    const auto pushVertices = [&](const std::vector<Vertex>& coords) {
        for (const auto& v : coords) {
            vertexBuffer.push_back({.x = v.x, .y = v.y});
        }
    };

    for (const auto& feature : features) {
        const auto& geom = feature.geometry;
        geometryTypes.push_back(geom.type);

        switch (geom.type) {
            case GeometryType::POINT:
                pushVertices(geom.coordinates);
                break;

            case GeometryType::LINESTRING:
                (containsPolygon ? numRings : numParts).push_back(static_cast<std::uint32_t>(geom.coordinates.size()));
                pushVertices(geom.coordinates);
                break;

            case GeometryType::POLYGON:
                numParts.push_back(static_cast<std::uint32_t>(geom.ringSizes.size()));
                for (const auto ringSize : geom.ringSizes) {
                    numRings.push_back(ringSize);
                }
                pushVertices(geom.coordinates);
                break;

            case GeometryType::MULTIPOINT:
                numGeometries.push_back(static_cast<std::uint32_t>(geom.coordinates.size()));
                pushVertices(geom.coordinates);
                break;

            case GeometryType::MULTILINESTRING:
                numGeometries.push_back(static_cast<std::uint32_t>(geom.parts.size()));
                for (const auto& part : geom.parts) {
                    (containsPolygon ? numRings : numParts).push_back(static_cast<std::uint32_t>(part.size()));
                    pushVertices(part);
                }
                break;

            case GeometryType::MULTIPOLYGON:
                numGeometries.push_back(static_cast<std::uint32_t>(geom.parts.size()));
                for (std::size_t p = 0; p < geom.parts.size(); ++p) {
                    const auto& rings = geom.partRingSizes[p];
                    numParts.push_back(static_cast<std::uint32_t>(rings.size()));
                    for (const auto ringSize : rings) {
                        numRings.push_back(ringSize);
                    }
                    pushVertices(geom.parts[p]);
                }
                break;
        }
    }
}

std::vector<Encoder::Feature> Encoder::Impl::sortFeatures(const std::vector<Encoder::Feature>& features) {
    if (features.empty()) {
        return {};
    }

    const auto firstType = features.front().geometry.type;
    if (firstType != GeometryType::POINT && firstType != GeometryType::LINESTRING) {
        return {};
    }
    const bool allSame = std::ranges::all_of(features,
                                             [firstType](const auto& f) { return f.geometry.type == firstType; });
    if (!allSame) {
        return {};
    }

    auto minVal = std::numeric_limits<std::int32_t>::max();
    auto maxVal = std::numeric_limits<std::int32_t>::min();
    for (const auto& f : features) {
        for (const auto& v : f.geometry.coordinates) {
            minVal = std::min({minVal, v.x, v.y});
            maxVal = std::max({maxVal, v.x, v.y});
        }
    }

    const util::HilbertCurve curve(minVal, maxVal);

    std::vector<std::uint32_t> hilbertIds(features.size());
    for (std::size_t i = 0; i < features.size(); ++i) {
        const auto& g = features[i].geometry;
        if (!g.coordinates.empty()) {
            const auto& v = features[i].geometry.coordinates[0];
            hilbertIds[i] = curve.encode({static_cast<float>(v.x), static_cast<float>(v.y)});
        }
    }

    std::vector<std::size_t> order(features.size());
    // NOLINTNEXTLINE(boost-use-ranges)
    std::iota(order.begin(), order.end(), 0);
    std::ranges::sort(order, [&](std::size_t a, std::size_t b) { return hilbertIds[a] < hilbertIds[b]; });

    std::vector<Feature> sorted(features.size());
    std::ranges::transform(order, sorted.begin(), [&](auto idx) { return features[idx]; });
    return sorted;
}

bool Encoder::Impl::allPolygons(const std::vector<Encoder::Feature>& features) {
    return !features.empty() && std::ranges::all_of(features, [](const auto& f) {
        return f.geometry.type == GeometryType::POLYGON || f.geometry.type == GeometryType::MULTIPOLYGON;
    });
}

void Encoder::Impl::tessellateFeatures(const std::vector<Feature>& features,
                                       std::vector<std::uint32_t>& numTriangles,
                                       std::vector<std::uint32_t>& indexBuffer) {
    using EarcutPoint = std::array<double, 2>;

    auto tessellateOnePolygon = [](const std::vector<Vertex>& coords,
                                   const std::vector<std::uint32_t>& ringSizes,
                                   std::uint32_t indexOffset) -> std::pair<std::uint32_t, std::vector<std::uint32_t>> {
        std::vector<std::vector<EarcutPoint>> polygon;
        std::size_t vertIdx = 0;
        for (auto ringSize : ringSizes) {
            std::vector<EarcutPoint> ring;
            ring.reserve(ringSize);
            for (std::uint32_t i = 0; i < ringSize; ++i) {
                ring.push_back({static_cast<double>(coords[vertIdx].x), static_cast<double>(coords[vertIdx].y)});
                ++vertIdx;
            }
            polygon.push_back(std::move(ring));
        }

        auto indices = mapbox::earcut<std::uint32_t>(polygon);
        if (indexOffset > 0) {
            std::ranges::for_each(indices, [=](auto& idx) { idx += indexOffset; });
        }
        return {static_cast<std::uint32_t>(indices.size() / 3), std::move(indices)};
    };

    for (const auto& feature : features) {
        const auto& geom = feature.geometry;
        if (geom.type == GeometryType::POLYGON) {
            auto [nTri, indices] = tessellateOnePolygon(geom.coordinates, geom.ringSizes, 0);
            numTriangles.push_back(nTri);
            indexBuffer.insert(indexBuffer.end(), indices.begin(), indices.end());
        } else if (geom.type == GeometryType::MULTIPOLYGON) {
            std::uint32_t totalTri = 0;
            std::uint32_t vertexOffset = 0;
            std::vector<std::uint32_t> allIndices;
            const auto totalVertices = util::sum(geom.parts, [](const auto& part) { return part.size(); });
            allIndices.reserve(totalVertices * 3);
            for (std::size_t p = 0; p < geom.parts.size(); ++p) {
                auto [nTri, indices] = tessellateOnePolygon(geom.parts[p], geom.partRingSizes[p], vertexOffset);
                totalTri += nTri;
                allIndices.insert(allIndices.end(), indices.begin(), indices.end());
                vertexOffset += static_cast<std::uint32_t>(geom.parts[p].size());
            }
            numTriangles.push_back(totalTri);
            indexBuffer.insert(indexBuffer.end(), allIndices.begin(), allIndices.end());
        }
    }
}

Encoder::Impl::ScalarPropertyCache Encoder::Impl::buildScalarPropertyCache(const FeatureTable& featureTable,
                                                                           const std::vector<Feature>& features) {
    ScalarPropertyCache scalarPropertyCache;
    scalarPropertyCache.reserve(featureTable.columns.size());
    for (const auto& column : featureTable.columns) {
        if (!column.isID() && !column.isGeometry() && !column.isStruct()) {
            scalarPropertyCache.emplace(column.name, std::vector<PropertyValuePtr>(features.size(), nullptr));
        }
    }

    for (std::size_t fi = 0; fi < features.size(); ++fi) {
        for (const auto& [key, value] : features[fi].properties) {
            auto it = scalarPropertyCache.find(key);
            if (it != scalarPropertyCache.end()) {
                it->second[fi] = &value;
            }
        }
    }

    return scalarPropertyCache;
}

std::vector<std::uint8_t> Encoder::Impl::assembleLayerBytes(const std::vector<std::uint8_t>& metadataBytes,
                                                            const EncodedChunks& bodyChunks) {
    const auto bodySize = util::sum(bodyChunks, [](const auto& chunk) { return chunk.size(); });

    std::vector<std::uint8_t> layerBytes;
    layerBytes.reserve(metadataBytes.size() + bodySize + 8 /* varint overhead */);
    util::encoding::encodeVarint(static_cast<std::uint32_t>(1), layerBytes);
    layerBytes.insert(layerBytes.end(), metadataBytes.begin(), metadataBytes.end());
    for (const auto& chunk : bodyChunks) {
        layerBytes.insert(layerBytes.end(), chunk.begin(), chunk.end());
    }
    return layerBytes;
}

std::vector<std::uint8_t> Encoder::Impl::encodeLayer(const Layer& layer, const EncoderConfig& config) {
    if (layer.features.empty()) {
        return {};
    }

    intEncoder.setDefaultEncodingOption(config.integerEncodingOption);

    const auto sortedStorage = config.sortFeatures ? sortFeatures(layer.features) : std::vector<Feature>{};
    const auto& features = sortedStorage.empty() ? layer.features : sortedStorage;

    const auto physicalTechnique = config.useFastPfor ? PhysicalLevelTechnique::FAST_PFOR
                                                      : PhysicalLevelTechnique::VARINT;

    const auto idStats = config.includeIds ? std::optional<IdStats>{collectIdStats(features)} : std::nullopt;
    const auto featureTable = buildMetadata(layer, config, idStats);
    const auto metadataBytes = encodeFeatureTable(featureTable);

    // Collect encoded chunks; concatenate once at the end to avoid repeated reallocation.
    EncodedChunks bodyChunks;
    const auto appendEncodedColumnChunks = [&](EncodedChunks encodedChunks) {
        for (auto& chunk : encodedChunks) {
            bodyChunks.push_back(std::move(chunk));
        }
    };
    const auto appendEncodedStreamSetChunks = [&](std::uint32_t numStreams, EncodedChunks encodedChunks) {
        std::vector<std::uint8_t> header;
        util::encoding::encodeVarint(numStreams, header);
        bodyChunks.push_back(std::move(header));
        for (auto& chunk : encodedChunks) {
            bodyChunks.push_back(std::move(chunk));
        }
    };
    const auto extractIds = [&]<typename TId>(auto&& convertId) {
        std::vector<std::optional<TId>> ids;
        ids.reserve(features.size());
        for (const auto& feature : features) {
            ids.push_back(convertId(feature));
        }
        return ids;
    };

    if (config.includeIds) {
        const auto hasLongId = idStats->hasLongId;
        const auto hasMissingId = idStats->hasMissingId;

        if (hasLongId) {
            const auto ids = extractIds.template operator()<std::uint64_t>(
                [](const auto& feature) -> std::optional<std::uint64_t> { return feature.id; });
            appendEncodedColumnChunks(PropertyEncoder::encodeUint64Column(ids, intEncoder, hasMissingId));
        } else {
            const auto ids = extractIds.template operator()<std::int32_t>(
                [](const auto& feature) -> std::optional<std::int32_t> {
                    return feature.id.has_value() ? std::optional<std::int32_t>{static_cast<std::int32_t>(*feature.id)}
                                                  : std::nullopt;
                });
            appendEncodedColumnChunks(
                PropertyEncoder::encodeInt32Column(ids, physicalTechnique, false, intEncoder, hasMissingId));
        }
    }

    std::vector<metadata::tileset::GeometryType> geometryTypes;
    std::vector<std::uint32_t> numGeometries, numParts, numRings;
    std::vector<GeometryEncoder::Vertex> vertexBuffer;
    collectGeometry(features, geometryTypes, numGeometries, numParts, numRings, vertexBuffer);

    const bool usePretessellation = config.preTessellate && allPolygons(features);
    const auto geometryIntegerEncodingOption = config.geometryEncodingOption.value_or(config.integerEncodingOption);
    const auto geometryTopologyIntegerEncodingOption = config.geometryTopologyEncodingOption.value_or(
        geometryIntegerEncodingOption);

    GeometryEncoder::EncodedGeometryColumn encodedGeom = [&] {
        if (usePretessellation) {
            std::vector<std::uint32_t> numTriangles;
            std::vector<std::uint32_t> indexBuffer;
            tessellateFeatures(features, numTriangles, indexBuffer);
            return GeometryEncoder::encodePretessellatedGeometryColumn(geometryTypes,
                                                                       numGeometries,
                                                                       numParts,
                                                                       numRings,
                                                                       vertexBuffer,
                                                                       numTriangles,
                                                                       indexBuffer,
                                                                       physicalTechnique,
                                                                       intEncoder,
                                                                       geometryIntegerEncodingOption,
                                                                       geometryTopologyIntegerEncodingOption,
                                                                       config.includeOutlines);
        }
        return GeometryEncoder::encodeGeometryColumn(geometryTypes,
                                                     numGeometries,
                                                     numParts,
                                                     numRings,
                                                     vertexBuffer,
                                                     physicalTechnique,
                                                     intEncoder,
                                                     geometryIntegerEncodingOption,
                                                     geometryTopologyIntegerEncodingOption,
                                                     config.enableMortonEncoding);
    }();

    appendEncodedStreamSetChunks(encodedGeom.numStreams, std::move(encodedGeom.chunks));

    // Cache properties by column once to avoid repeated per-feature linear searches.
    const auto scalarPropertyCache = buildScalarPropertyCache(featureTable, features);

    for (const auto& column : featureTable.columns) {
        if (column.isID() || column.isGeometry()) {
            continue;
        }

        if (column.isStruct()) {
            const auto& complex = column.getComplexType();
            const auto& rootName = column.name;
            const auto* const sourceKey = resolveStructSourceKey(features, column);
            const auto numChildren = complex.children.size();
            const auto resolveStructChildValue = [&](const Encoder::StructValue& sv,
                                                     const std::string& childName) -> const std::string* {
                if (auto childIt = sv.find(childName); childIt != sv.end()) {
                    return &childIt->second;
                }
                if (auto prefixedChildIt = sv.find(rootName + childName); prefixedChildIt != sv.end()) {
                    return &prefixedChildIt->second;
                }
                return nullptr;
            };

            std::vector<const Encoder::StructValue*> structValues(features.size(), nullptr);
            for (std::size_t fi = 0; fi < features.size(); ++fi) {
                const auto it = sourceKey ? features[fi].properties.find(*sourceKey)
                                          : features[fi].properties.find(rootName);
                if (it != features[fi].properties.end() && std::holds_alternative<Encoder::StructValue>(it->second)) {
                    structValues[fi] = &std::get<Encoder::StructValue>(it->second);
                }
            }

            // Build one optional<string_view> column per child, pointing directly into live feature data.
            // This eliminates the previous ownedStrings (string copies), viewStorage, and childPresent intermediates.
            std::vector<std::vector<std::optional<std::string_view>>> optCols(numChildren);
            for (std::size_t c = 0; c < numChildren; ++c) {
                optCols[c].reserve(features.size());
            }
            for (std::size_t fi = 0; fi < features.size(); ++fi) {
                const auto* sv = structValues[fi];
                for (std::size_t c = 0; c < numChildren; ++c) {
                    const auto* childValue = sv ? resolveStructChildValue(*sv, complex.children[c].name) : nullptr;
                    optCols[c].push_back(childValue ? std::optional<std::string_view>{*childValue} : std::nullopt);
                }
            }

            auto result = StringEncoder::encodeSharedDictionary(optCols, physicalTechnique, intEncoder, config.useFsst);

            appendEncodedStreamSetChunks(result.numStreams, std::move(result.chunks));
            continue;
        }

        const auto& scalarCol = column.getScalarType();
        const auto scalarType = scalarCol.getPhysicalType();
        const auto& colName = column.name;
        const auto cachedIt = scalarPropertyCache.find(colName);
        if (cachedIt == scalarPropertyCache.end()) {
            throwInvalidType(); // GCOVR_EXCL_LINE
        }
        const auto& cachedPropertyValues = cachedIt->second;

        const auto extractSeparatedColumn =
            [&]<typename T>(
                auto&& visitor, std::vector<bool>& presentValues, std::vector<T>& dataValues, bool& hasNull) {
                presentValues.clear();
                presentValues.reserve(features.size());
                dataValues.clear();
                dataValues.reserve(features.size());
                hasNull = false;
                for (const auto* propertyValue : cachedPropertyValues) {
                    if (propertyValue != nullptr) {
                        presentValues.push_back(true);
                        dataValues.push_back(std::visit(visitor, *propertyValue));
                    } else {
                        presentValues.push_back(false);
                        hasNull = true;
                    }
                }
            };

        const auto encodeSeparatedColumn = [&]<typename T>(auto&& visitor, auto&& encodeColumn) -> EncodedChunks {
            std::vector<bool> presentValues;
            std::vector<T> dataValues;
            bool hasNull = false;
            extractSeparatedColumn.template operator()<T>(visitor, presentValues, dataValues, hasNull);
            return encodeColumn(std::span<const T>{dataValues}, presentValues, hasNull);
        };

        EncodedChunks encodedChunks;
        switch (scalarType) {
            case ScalarType::BOOLEAN:
                encodedChunks = encodeSeparatedColumn.template operator()<std::uint8_t>(
                    util::overloaded{
                        [](bool v) -> std::uint8_t { return static_cast<std::uint8_t>(v); },
                        // the type is already determined by column metadata, so the catch-all arms are dead by
                        // construction
                        [](auto&) -> std::uint8_t { throwInvalidType(); }, // GCOVR_EXCL_LINE
                    },
                    [&](auto dataValues, const auto& presentValues, bool hasNull) {
                        return PropertyEncoder::encodeDataColumn(dataValues,
                                                                 presentValues,
                                                                 hasNull,
                                                                 column.nullable,
                                                                 [](std::span<const std::uint8_t> input) {
                                                                     return BooleanEncoder::encodeBooleanStream(
                                                                         input,
                                                                         metadata::stream::PhysicalStreamType::DATA);
                                                                 });
                    });
                break;
            case ScalarType::INT_32:
                encodedChunks = encodeSeparatedColumn.template operator()<std::int32_t>(
                    util::overloaded{
                        [](std::int32_t v) -> std::int32_t { return v; },
                        [](std::int64_t v) -> std::int32_t { return static_cast<std::int32_t>(v); },
                        [](auto&) -> std::int32_t { throwInvalidType(); }, // GCOVR_EXCL_LINE
                    },
                    [&](auto dataValues, const auto& presentValues, bool hasNull) {
                        return PropertyEncoder::encodeDataColumn(dataValues,
                                                                 presentValues,
                                                                 hasNull,
                                                                 column.nullable,
                                                                 [&](std::span<const std::int32_t> input) {
                                                                     return intEncoder.encodeIntStream(
                                                                         input,
                                                                         physicalTechnique,
                                                                         true,
                                                                         metadata::stream::PhysicalStreamType::DATA,
                                                                         std::nullopt);
                                                                 });
                    });
                break;
            case ScalarType::UINT_32:
                encodedChunks = encodeSeparatedColumn.template operator()<std::uint32_t>(
                    util::overloaded{
                        [](std::uint32_t v) -> std::uint32_t { return v; },
                        [](std::int32_t v) -> std::uint32_t { return static_cast<std::uint32_t>(v); },
                        [](auto&) -> std::uint32_t { throwInvalidType(); }, // GCOVR_EXCL_LINE
                    },
                    [&](auto dataValues, const auto& presentValues, bool hasNull) {
                        return PropertyEncoder::encodeDataColumn(
                            dataValues,
                            presentValues,
                            hasNull,
                            column.nullable,
                            [&](std::span<const std::uint32_t> input) {
                                return intEncoder.encodeUint32Stream(
                                    input, physicalTechnique, metadata::stream::PhysicalStreamType::DATA, std::nullopt);
                            });
                    });
                break;
            case ScalarType::INT_64:
                encodedChunks = encodeSeparatedColumn.template operator()<std::int64_t>(
                    util::overloaded{
                        [](std::int64_t v) -> std::int64_t { return v; },
                        [](std::int32_t v) -> std::int64_t { return v; },
                        [](auto&) -> std::int64_t { throwInvalidType(); }, // GCOVR_EXCL_LINE
                    },
                    [&](auto dataValues, const auto& presentValues, bool hasNull) {
                        return PropertyEncoder::encodeDataColumn(
                            dataValues,
                            presentValues,
                            hasNull,
                            column.nullable,
                            [&](std::span<const std::int64_t> input) {
                                return intEncoder.encodeLongStream(
                                    input, true, metadata::stream::PhysicalStreamType::DATA, std::nullopt);
                            });
                    });
                break;
            case ScalarType::UINT_64:
                encodedChunks = encodeSeparatedColumn.template operator()<std::uint64_t>(
                    util::overloaded{
                        [](std::uint64_t v) -> std::uint64_t { return v; },
                        [](std::int64_t v) -> std::uint64_t { return static_cast<std::uint64_t>(v); },
                        [](auto&) -> std::uint64_t { throwInvalidType(); }, // GCOVR_EXCL_LINE
                    },
                    [&](auto dataValues, const auto& presentValues, bool hasNull) {
                        return PropertyEncoder::encodeDataColumn(
                            dataValues,
                            presentValues,
                            hasNull,
                            column.nullable,
                            [&](std::span<const std::uint64_t> input) {
                                return intEncoder.encodeUint64Stream(
                                    input, metadata::stream::PhysicalStreamType::DATA, std::nullopt);
                            });
                    });
                break;
            case ScalarType::FLOAT:
                encodedChunks = encodeSeparatedColumn.template operator()<float>(
                    util::overloaded{
                        [](float v) -> float { return v; },
                        [](double v) -> float { return static_cast<float>(v); },
                        [](auto&) -> float { throwInvalidType(); }, // GCOVR_EXCL_LINE
                    },
                    [&](auto dataValues, const auto& presentValues, bool hasNull) {
                        return PropertyEncoder::encodeDataColumn(
                            dataValues, presentValues, hasNull, column.nullable, [](std::span<const float> input) {
                                return FloatEncoder::encodeStream(input);
                            });
                    });
                break;
            case ScalarType::DOUBLE:
                encodedChunks = encodeSeparatedColumn.template operator()<double>(
                    util::overloaded{
                        [](double v) -> double { return v; },
                        [](float v) -> double { return static_cast<double>(v); },
                        [](auto&) -> double { throwInvalidType(); }, // GCOVR_EXCL_LINE
                    },
                    [&](auto dataValues, const auto& presentValues, bool hasNull) {
                        return PropertyEncoder::encodeDataColumn(
                            dataValues, presentValues, hasNull, column.nullable, [](std::span<const double> input) {
                                return FloatEncoder::encodeStream(input);
                            });
                    });
                break;
            case ScalarType::STRING: {
                std::vector<std::string> coercedStrings;
                coercedStrings.reserve(features.size());
                std::vector<bool> presentValues;
                presentValues.reserve(features.size());
                std::vector<std::string_view> dataValues;
                dataValues.reserve(features.size());
                for (const auto* propertyValue : cachedPropertyValues) {
                    if (propertyValue != nullptr) {
                        presentValues.push_back(true);
                        dataValues.push_back(std::visit(
                            util::overloaded{
                                [](const std::string& v) -> std::string_view { return std::string_view{v}; },
                                [](const Encoder::StructValue&) -> std::string_view { return std::string_view{}; },
                                [&](auto v) -> std::string_view {
                                    auto& coerced = coercedStrings.emplace_back(std::to_string(v));
                                    return std::string_view{coerced};
                                },
                            },
                            *propertyValue));
                    } else {
                        presentValues.push_back(false);
                    }
                }
                encodedChunks = PropertyEncoder::encodeStringColumn(
                    dataValues, presentValues, physicalTechnique, intEncoder, config.useFsst, column.nullable);
                break;
            }
            default:
                throwInvalidType(); // GCOVR_EXCL_LINE
        }
        appendEncodedColumnChunks(std::move(encodedChunks));
    }

    return assembleLayerBytes(metadataBytes, bodyChunks);
}

Encoder::Encoder()
    : impl(std::make_unique<Impl>()) {}

Encoder::~Encoder() noexcept = default;

std::vector<std::uint8_t> Encoder::encode(const std::vector<Layer>& layers, const EncoderConfig& config) const {
    // Accumulate (size-varint, layer-bytes) pairs, then concatenate once.
    struct LayerChunk {
        std::vector<std::uint8_t> sizeVarint;
        std::vector<std::uint8_t> data;
    };
    std::vector<LayerChunk> chunks;
    chunks.reserve(layers.size());
    std::size_t totalSize = 0;
    for (const auto& layer : layers) {
        auto layerBytes = impl->encodeLayer(layer, config);
        if (layerBytes.empty()) {
            continue;
        }
        std::vector<std::uint8_t> sizeVarint;
        util::encoding::encodeVarint(static_cast<std::uint32_t>(layerBytes.size()), sizeVarint);
        totalSize += sizeVarint.size() + layerBytes.size();
        chunks.push_back(LayerChunk{.sizeVarint = std::move(sizeVarint), .data = std::move(layerBytes)});
    }
    std::vector<std::uint8_t> result;
    result.reserve(totalSize);
    for (const auto& chunk : chunks) {
        result.insert(result.end(), chunk.sizeVarint.begin(), chunk.sizeVarint.end());
        result.insert(result.end(), chunk.data.begin(), chunk.data.end());
    }
    return result;
}

} // namespace mlt
