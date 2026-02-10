#include <mlt/encoder.hpp>

#include <mlt/encode/boolean.hpp>
#include <mlt/encode/geometry.hpp>
#include <mlt/encode/int.hpp>
#include <mlt/encode/property.hpp>
#include <mlt/encode/string.hpp>
#include <mlt/metadata/tileset.hpp>
#include <mlt/metadata/type_map.hpp>
#include <mlt/util/encoding/varint.hpp>
#include <mlt/util/hilbert_curve.hpp>
#include <mlt/util/stl.hpp>

#include <mapbox/earcut.hpp>

#include <algorithm>
#include <array>
#include <numeric>
#include <stdexcept>
#include <unordered_set>

namespace mlt {

using namespace encoder;
using namespace metadata::tileset;
using namespace metadata::stream;

struct Encoder::Impl {
    IntegerEncoder intEncoder;

    std::vector<std::uint8_t> encodeLayer(const Layer& layer, const EncoderConfig& config);

    FeatureTable buildMetadata(const Layer& layer, const EncoderConfig& config);

    static void collectGeometry(const std::vector<Feature>& features,
                                std::vector<GeometryType>& geometryTypes,
                                std::vector<std::uint32_t>& numGeometries,
                                std::vector<std::uint32_t>& numParts,
                                std::vector<std::uint32_t>& numRings,
                                std::vector<GeometryEncoder::Vertex>& vertexBuffer);

    static bool canSort(const std::vector<Encoder::Feature>& features);
    static std::vector<Encoder::Feature> sortFeatures(const std::vector<Encoder::Feature>& features);

    static bool allPolygons(const std::vector<Encoder::Feature>& features);
    static void tessellateFeatures(const std::vector<Feature>& features,
                                   std::vector<std::uint32_t>& numTriangles,
                                   std::vector<std::uint32_t>& indexBuffer);
};

FeatureTable Encoder::Impl::buildMetadata(const Layer& layer, const EncoderConfig& config) {
    FeatureTable table;
    table.name = layer.name;
    table.extent = layer.extent;

    if (config.includeIds) {
        // Use 64-bit when any ID exceeds INT32_MAX: delta encoding accumulates in
        // int32_t, so uint32 values with bit 31 set would sign-extend on widening.
        bool hasLongId = std::any_of(layer.features.begin(), layer.features.end(), [](const auto& f) {
            return f.id > std::numeric_limits<std::int32_t>::max();
        });

        Column idColumn;
        idColumn.nullable = false;
        idColumn.columnScope = ColumnScope::FEATURE;
        idColumn.type = ScalarColumn{.type = LogicalScalarType::ID, .hasLongID = hasLongId};
        table.columns.push_back(std::move(idColumn));
    }

    Column geomColumn;
    geomColumn.nullable = false;
    geomColumn.columnScope = ColumnScope::FEATURE;
    geomColumn.type = ComplexColumn{.type = ComplexType::GEOMETRY};
    table.columns.push_back(std::move(geomColumn));

    struct ColumnInfo {
        ScalarType type;
        bool nullable;
    };
    std::map<std::string, ColumnInfo> scalarColumns;
    std::map<std::string, std::set<std::string>> structColumns;

    for (const auto& feature : layer.features) {
        for (const auto& [key, value] : feature.properties) {
            if (std::holds_alternative<Encoder::StructValue>(value)) {
                const auto& sv = std::get<Encoder::StructValue>(value);
                auto& children = structColumns[key];
                for (const auto& [childName, _] : sv) {
                    children.insert(childName);
                }
                continue;
            }

            auto scalarType = std::visit(util::overloaded{
                                             [](bool) { return ScalarType::BOOLEAN; },
                                             [](std::int32_t) { return ScalarType::INT_32; },
                                             [](std::int64_t) { return ScalarType::INT_64; },
                                             [](std::uint32_t) { return ScalarType::UINT_32; },
                                             [](std::uint64_t) { return ScalarType::UINT_64; },
                                             [](float) { return ScalarType::FLOAT; },
                                             [](double) { return ScalarType::DOUBLE; },
                                             [](const std::string&) { return ScalarType::STRING; },
                                             [](const Encoder::StructValue&) { return ScalarType::STRING; },
                                         },
                                         value);

            auto [it, inserted] = scalarColumns.try_emplace(key, ColumnInfo{scalarType, false});
            if (!inserted) {
                auto& existing = it->second;
                if (existing.type != scalarType) {
                    if ((existing.type == ScalarType::INT_32 && scalarType == ScalarType::INT_64) ||
                        (existing.type == ScalarType::FLOAT && scalarType == ScalarType::DOUBLE)) {
                        existing.type = scalarType;
                    } else if ((existing.type == ScalarType::INT_64 && scalarType == ScalarType::INT_32) ||
                               (existing.type == ScalarType::DOUBLE && scalarType == ScalarType::FLOAT)) {
                    } else {
                        existing.type = ScalarType::STRING;
                    }
                }
            }
        }
    }

    for (auto& [key, info] : scalarColumns) {
        if (info.type == ScalarType::STRING) {
            info.nullable = true;
            continue;
        }
        info.nullable = std::ranges::any_of(layer.features,
                                            [&](const auto& feature) { return !feature.properties.contains(key); });
    }

    for (const auto& [name, info] : scalarColumns) {
        Column col;
        col.name = name;
        col.nullable = info.nullable;
        col.columnScope = ColumnScope::FEATURE;
        col.type = ScalarColumn{.type = info.type};
        table.columns.push_back(std::move(col));
    }

    for (const auto& [name, childNames] : structColumns) {
        Column col;
        col.name = name;
        col.nullable = false;
        col.columnScope = ColumnScope::FEATURE;

        ComplexColumn complex;
        complex.type = ComplexType::STRUCT;
        for (const auto& childName : childNames) {
            Column child;
            child.name = childName;
            child.nullable = true;
            child.columnScope = ColumnScope::FEATURE;
            child.type = ScalarColumn{.type = ScalarType::STRING};
            complex.children.push_back(std::move(child));
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
    using GT = metadata::tileset::GeometryType;

    const bool containsPolygon = std::any_of(features.begin(), features.end(), [](const auto& f) {
        return f.geometry.type == GT::POLYGON || f.geometry.type == GT::MULTIPOLYGON;
    });

    auto pushVertices = [&](const std::vector<Vertex>& coords) {
        for (const auto& v : coords) vertexBuffer.push_back({v.x, v.y});
    };

    for (const auto& feature : features) {
        const auto& geom = feature.geometry;
        geometryTypes.push_back(geom.type);

        switch (geom.type) {
            case GT::POINT:
                pushVertices(geom.coordinates);
                break;

            case GT::LINESTRING:
                (containsPolygon ? numRings : numParts).push_back(static_cast<std::uint32_t>(geom.coordinates.size()));
                pushVertices(geom.coordinates);
                break;

            case GT::POLYGON:
                numParts.push_back(static_cast<std::uint32_t>(geom.ringSizes.size()));
                for (auto ringSize : geom.ringSizes) {
                    numRings.push_back(ringSize);
                }
                pushVertices(geom.coordinates);
                break;

            case GT::MULTIPOINT:
                numGeometries.push_back(static_cast<std::uint32_t>(geom.coordinates.size()));
                pushVertices(geom.coordinates);
                break;

            case GT::MULTILINESTRING:
                numGeometries.push_back(static_cast<std::uint32_t>(geom.parts.size()));
                for (const auto& part : geom.parts) {
                    (containsPolygon ? numRings : numParts).push_back(static_cast<std::uint32_t>(part.size()));
                    pushVertices(part);
                }
                break;

            case GT::MULTIPOLYGON:
                numGeometries.push_back(static_cast<std::uint32_t>(geom.parts.size()));
                for (std::size_t p = 0; p < geom.parts.size(); ++p) {
                    const auto& rings = geom.partRingSizes[p];
                    numParts.push_back(static_cast<std::uint32_t>(rings.size()));
                    for (auto ringSize : rings) {
                        numRings.push_back(ringSize);
                    }
                    pushVertices(geom.parts[p]);
                }
                break;
        }
    }
}

bool Encoder::Impl::canSort(const std::vector<Encoder::Feature>& features) {
    using GT = metadata::tileset::GeometryType;
    if (features.empty()) return false;

    auto firstType = features[0].geometry.type;
    bool allSame = std::all_of(
        features.begin(), features.end(), [firstType](const auto& f) { return f.geometry.type == firstType; });
    if (!allSame) return false;

    return firstType == GT::POINT || firstType == GT::LINESTRING;
}

std::vector<Encoder::Feature> Encoder::Impl::sortFeatures(const std::vector<Encoder::Feature>& features) {
    auto minVal = std::numeric_limits<std::int32_t>::max();
    auto maxVal = std::numeric_limits<std::int32_t>::min();
    for (const auto& f : features) {
        for (const auto& v : f.geometry.coordinates) {
            minVal = std::min({minVal, v.x, v.y});
            maxVal = std::max({maxVal, v.x, v.y});
        }
    }

    util::HilbertCurve curve(minVal, maxVal);

    std::vector<std::uint32_t> hilbertIds(features.size());
    for (std::size_t i = 0; i < features.size(); ++i) {
        const auto& v = features[i].geometry.coordinates[0];
        hilbertIds[i] = curve.encode({static_cast<float>(v.x), static_cast<float>(v.y)});
    }

    std::vector<std::size_t> order(features.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return hilbertIds[a] < hilbertIds[b]; });

    std::vector<Feature> sorted(features.size());
    std::transform(order.begin(), order.end(), sorted.begin(), [&](auto idx) { return features[idx]; });
    return sorted;
}

bool Encoder::Impl::allPolygons(const std::vector<Encoder::Feature>& features) {
    return !features.empty() && std::all_of(features.begin(), features.end(), [](const auto& f) {
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

std::vector<std::uint8_t> Encoder::Impl::encodeLayer(const Layer& layer, const EncoderConfig& config) {
    if (layer.features.empty()) {
        return {};
    }

    const bool shouldSort = config.sortFeatures && canSort(layer.features);
    const auto sortedStorage = shouldSort ? sortFeatures(layer.features) : std::vector<Feature>{};
    const auto& features = shouldSort ? sortedStorage : layer.features;

    auto physicalTechnique = config.useFastPfor ? PhysicalLevelTechnique::FAST_PFOR : PhysicalLevelTechnique::VARINT;

    auto featureTable = buildMetadata(layer, config);
    auto metadataBytes = encodeFeatureTable(featureTable);

    std::vector<std::uint8_t> bodyBytes;

    if (config.includeIds) {
        bool hasLongId = std::any_of(features.begin(), features.end(), [](const auto& f) {
            return f.id > std::numeric_limits<std::int32_t>::max();
        });

        if (hasLongId) {
            std::vector<std::uint64_t> ids(features.size());
            std::transform(features.begin(), features.end(), ids.begin(), [](const auto& f) { return f.id; });
            auto encoded = PropertyEncoder::encodeUint64Column(ids, intEncoder);
            bodyBytes.insert(bodyBytes.end(), encoded.begin(), encoded.end());
        } else {
            std::vector<std::uint32_t> ids(features.size());
            std::transform(features.begin(), features.end(), ids.begin(), [](const auto& f) {
                return static_cast<std::uint32_t>(f.id);
            });
            auto encoded = PropertyEncoder::encodeUint32Column(ids, physicalTechnique, intEncoder);
            bodyBytes.insert(bodyBytes.end(), encoded.begin(), encoded.end());
        }
    }

    std::vector<metadata::tileset::GeometryType> geometryTypes;
    std::vector<std::uint32_t> numGeometries, numParts, numRings;
    std::vector<GeometryEncoder::Vertex> vertexBuffer;
    collectGeometry(features, geometryTypes, numGeometries, numParts, numRings, vertexBuffer);

    const bool usePretessellation = config.preTessellate && allPolygons(features);

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
                                                                       true);
        }
        return GeometryEncoder::encodeGeometryColumn(
            geometryTypes, numGeometries, numParts, numRings, vertexBuffer, physicalTechnique, intEncoder);
    }();

    util::encoding::encodeVarint(encodedGeom.numStreams, bodyBytes);
    bodyBytes.insert(bodyBytes.end(), encodedGeom.encodedValues.begin(), encodedGeom.encodedValues.end());

    for (const auto& column : featureTable.columns) {
        if (column.isID() || column.isGeometry()) {
            continue;
        }

        if (column.isStruct()) {
            const auto& complex = column.getComplexType();
            const auto& rootName = column.name;
            const auto numChildren = complex.children.size();

            std::vector<std::vector<std::string>> ownedStrings(numChildren);
            std::vector<std::vector<std::string_view>> viewStorage(numChildren);

            for (std::size_t c = 0; c < numChildren; ++c) {
                ownedStrings[c].reserve(features.size());
                viewStorage[c].reserve(features.size());
            }

            for (const auto& f : features) {
                auto it = f.properties.find(rootName);
                const Encoder::StructValue* sv = nullptr;
                if (it != f.properties.end() && std::holds_alternative<Encoder::StructValue>(it->second)) {
                    sv = &std::get<Encoder::StructValue>(it->second);
                }
                for (std::size_t c = 0; c < numChildren; ++c) {
                    if (sv) {
                        auto childIt = sv->find(complex.children[c].name);
                        if (childIt != sv->end()) {
                            ownedStrings[c].push_back(childIt->second);
                            continue;
                        }
                    }
                    ownedStrings[c].emplace_back();
                }
            }

            for (std::size_t c = 0; c < numChildren; ++c) {
                for (const auto& s : ownedStrings[c]) {
                    viewStorage[c].emplace_back(s);
                }
            }

            std::vector<std::vector<const std::string_view*>> sharedCols(numChildren);
            for (std::size_t c = 0; c < numChildren; ++c) {
                sharedCols[c].reserve(features.size());
                for (std::size_t fi = 0; fi < features.size(); ++fi) {
                    auto it = features[fi].properties.find(rootName);
                    const Encoder::StructValue* sv = nullptr;
                    if (it != features[fi].properties.end() &&
                        std::holds_alternative<Encoder::StructValue>(it->second)) {
                        sv = &std::get<Encoder::StructValue>(it->second);
                    }
                    if (sv && sv->contains(complex.children[c].name)) {
                        sharedCols[c].push_back(&viewStorage[c][fi]);
                    } else {
                        sharedCols[c].push_back(nullptr);
                    }
                }
            }

            auto result = StringEncoder::encodeSharedDictionary(sharedCols, physicalTechnique, intEncoder);

            util::encoding::encodeVarint(result.numStreams, bodyBytes);
            bodyBytes.insert(bodyBytes.end(), result.data.begin(), result.data.end());
            continue;
        }

        const auto& scalarCol = column.getScalarType();
        const auto scalarType = scalarCol.getPhysicalType();
        const auto& colName = column.name;

        auto extractColumn = [&]<typename T>(auto&& visitor) {
            std::vector<std::optional<T>> values;
            values.reserve(features.size());
            for (const auto& f : features) {
                auto it = f.properties.find(colName);
                if (it != f.properties.end()) {
                    values.push_back(std::visit(visitor, it->second));
                } else {
                    values.push_back(std::nullopt);
                }
            }
            return values;
        };

        std::vector<std::uint8_t> encoded;
        switch (scalarType) {
            case ScalarType::BOOLEAN:
                encoded = PropertyEncoder::encodeBooleanColumn(extractColumn.template operator()<bool>(util::overloaded{
                    [](bool v) -> bool { return v; },
                    [](auto) -> bool { return false; },
                }));
                break;
            case ScalarType::INT_32:
                encoded = PropertyEncoder::encodeInt32Column(
                    extractColumn.template operator()<std::int32_t>(util::overloaded{
                        [](std::int32_t v) -> std::int32_t { return v; },
                        [](std::int64_t v) -> std::int32_t { return static_cast<std::int32_t>(v); },
                        [](auto) -> std::int32_t { return 0; },
                    }),
                    physicalTechnique,
                    true,
                    intEncoder);
                break;
            case ScalarType::UINT_32:
                encoded = PropertyEncoder::encodeInt32Column(
                    extractColumn.template operator()<std::int32_t>(util::overloaded{
                        [](std::uint32_t v) -> std::int32_t { return static_cast<std::int32_t>(v); },
                        [](std::int32_t v) -> std::int32_t { return v; },
                        [](auto) -> std::int32_t { return 0; },
                    }),
                    physicalTechnique,
                    false,
                    intEncoder);
                break;
            case ScalarType::INT_64:
                encoded = PropertyEncoder::encodeInt64Column(
                    extractColumn.template operator()<std::int64_t>(util::overloaded{
                        [](std::int64_t v) -> std::int64_t { return v; },
                        [](std::int32_t v) -> std::int64_t { return v; },
                        [](auto) -> std::int64_t { return 0; },
                    }),
                    true,
                    intEncoder);
                break;
            case ScalarType::UINT_64:
                encoded = PropertyEncoder::encodeInt64Column(
                    extractColumn.template operator()<std::int64_t>(util::overloaded{
                        [](std::uint64_t v) -> std::int64_t { return static_cast<std::int64_t>(v); },
                        [](std::int64_t v) -> std::int64_t { return v; },
                        [](auto) -> std::int64_t { return 0; },
                    }),
                    false,
                    intEncoder);
                break;
            case ScalarType::FLOAT:
                encoded = PropertyEncoder::encodeFloatColumn(extractColumn.template operator()<float>(util::overloaded{
                    [](float v) -> float { return v; },
                    [](double v) -> float { return static_cast<float>(v); },
                    [](auto) -> float { return 0.0f; },
                }));
                break;
            case ScalarType::DOUBLE:
                encoded = PropertyEncoder::encodeDoubleColumn(
                    extractColumn.template operator()<double>(util::overloaded{
                        [](double v) -> double { return v; },
                        [](float v) -> double { return static_cast<double>(v); },
                        [](auto) -> double { return 0.0; },
                    }));
                break;
            case ScalarType::STRING: {
                std::vector<std::string> ownedStrings;
                ownedStrings.reserve(features.size());
                std::vector<std::optional<std::string_view>> values;
                values.reserve(features.size());
                for (const auto& f : features) {
                    auto it = f.properties.find(colName);
                    if (it != f.properties.end()) {
                        auto& owned = ownedStrings.emplace_back(
                            std::visit(util::overloaded{
                                           [](const std::string& v) -> std::string { return v; },
                                           [](const Encoder::StructValue&) -> std::string { return {}; },
                                           [](auto v) -> std::string { return std::to_string(v); },
                                       },
                                       it->second));
                        values.push_back(std::string_view{owned});
                    } else {
                        values.push_back(std::nullopt);
                    }
                }
                encoded = PropertyEncoder::encodeStringColumn(values, physicalTechnique, intEncoder);
                break;
            }
            default:
                throw std::runtime_error("Unsupported property type for column: " + colName);
        }
        bodyBytes.insert(bodyBytes.end(), encoded.begin(), encoded.end());
    }

    std::vector<std::uint8_t> layerBytes;
    util::encoding::encodeVarint(static_cast<std::uint32_t>(1), layerBytes);
    layerBytes.insert(layerBytes.end(), metadataBytes.begin(), metadataBytes.end());
    layerBytes.insert(layerBytes.end(), bodyBytes.begin(), bodyBytes.end());
    return layerBytes;
}

Encoder::Encoder()
    : impl(std::make_unique<Impl>()) {}

Encoder::~Encoder() noexcept = default;

std::vector<std::uint8_t> Encoder::encode(const std::vector<Layer>& layers, const EncoderConfig& config) {
    std::vector<std::uint8_t> result;
    for (const auto& layer : layers) {
        auto layerBytes = impl->encodeLayer(layer, config);
        if (layerBytes.empty()) {
            continue;
        }
        util::encoding::encodeVarint(static_cast<std::uint32_t>(layerBytes.size()), result);
        result.insert(result.end(), layerBytes.begin(), layerBytes.end());
    }
    return result;
}

} // namespace mlt
