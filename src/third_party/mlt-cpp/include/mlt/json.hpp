#pragma once

#include <sstream>
#if MLT_WITH_JSON
#include <mlt/common.hpp>
#include <mlt/coordinate.hpp>
#include <mlt/feature.hpp>
#include <mlt/geometry.hpp>
#include <mlt/layer.hpp>
#include <mlt/projection.hpp>
#include <mlt/tile.hpp>

#include <nlohmann/json.hpp>

#include <cmath>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace mlt::json {

using json = nlohmann::json;

namespace detail {

#pragma region JSON utils
/// Create a json array object with pre-allocated space
inline json buildArray(std::size_t reservedSize) {
    json array = json::array();
    assert(array.is_array());
    array.get_ref<nlohmann::json::array_t&>().reserve(reservedSize);
    return array;
}

/// Add to a json array the result of a function applied to each element in a range
template <typename TRange, typename TFunc>
    requires requires(TFunc f, std::ranges::range_rvalue_reference_t<TRange> v) {
        { f(v) } -> std::same_as<json>;
    }
inline json append(const TRange& sourceRange, json&& array, const TFunc& transform) {
    assert(array.is_array());
    std::ranges::transform(sourceRange, std::back_inserter(array), transform);
    return std::move(array);
}

/// Convert a collection range into a json array by applying the given function to each element
template <typename TRange, typename TFunc>
    requires requires(TFunc f, std::ranges::range_rvalue_reference_t<TRange> v) {
        { f(v) } -> std::same_as<json>;
    }
inline json buildArray(const TRange& sourceRange, const TFunc& transform) {
    return append(sourceRange, buildArray(std::ranges::size(sourceRange)), transform);
}
#pragma endregion JSON utils

#pragma region Geometry
/// Build the coordinate representation for a single coordinate that has already been projected
inline json buildProjectedCoordinateArray(const Coordinate& coord) {
    return json::array({coord.x, coord.y});
}

/// Build the coordinate representation for a single coordinate
inline json buildCoordinateArray(const Coordinate& coord, const Projection& projection) {
    return buildProjectedCoordinateArray(projection.project(coord));
}

/// Build the coordinate representation for a collection of coordinates
inline json buildCoordinatesArray(const CoordVec& coords, const Projection& projection) {
    return buildArray(coords, [&](const auto& coord) { return buildCoordinateArray(coord, projection); });
}

/// Build the coordinate representation for a polygon, consisting of the rings concatenated to the shell
inline json buildPolygonCoords(const std::vector<CoordVec>& polyRings, const Projection& projection) {
    auto result = buildArray(polyRings.size());
    return append(polyRings, std::move(result), [&](const auto& lineString) {
        return buildCoordinatesArray(lineString, projection);
    });
}

/// Create the value type for the "geometry" entry in a GeoJSON feature with the given coordinate representation
inline json buildGeometryElement(std::string_view type, json&& coords) {
    return {
        {"type", type},
        {"coordinates", std::move(coords)},
    };
}

inline json buildGeometryElement(const geometry::Point& point, const Projection& projection, bool geoJSON) {
    if (geoJSON) {
        return {
            {"type", "Point"},
            {"coordinates", buildCoordinateArray(point.getCoordinate(), projection)},
        };
    }
    std::ostringstream ss;
    ss << "POINT (" << point.getCoordinate().x << " " << point.getCoordinate().y << ")";
    return ss.str();
}

inline json buildGeometryElement(const geometry::LineString& line, const Projection& projection, bool geoJSON) {
    if (geoJSON) {
        return buildGeometryElement("LineString", buildCoordinatesArray(line.getCoordinates(), projection));
    }
    std::ostringstream ss;
    ss << "LINESTRING (";
    const auto& coords = line.getCoordinates();
    for (std::size_t i = 0, n = coords.size(); i < n; ++i) {
        ss << (i == 0 ? "" : ", ") << coords[i].x << " " << coords[i].y;
    }
    ss << ")";
    return ss.str();
}

inline json buildGeometryElement(const geometry::LinearRing& line, const Projection& projection, bool geoJSON) {
    if (geoJSON) {
        return buildGeometryElement("LineString", buildCoordinatesArray(line.getCoordinates(), projection));
    }
    std::ostringstream ss;
    ss << "LINESTRING (";
    const auto& coords = line.getCoordinates();
    for (std::size_t i = 0; i < coords.size(); ++i) {
        ss << (i == 0 ? "" : ", ") << coords[i].x << " " << coords[i].y;
    }
    ss << ")";
    return ss.str();
}

inline json buildGeometryElement(const geometry::MultiPoint& points, const Projection& projection, bool geoJSON) {
    if (geoJSON) {
        return buildGeometryElement("MultiPoint", buildCoordinatesArray(points.getCoordinates(), projection));
    }
    std::ostringstream ss;
    ss << "MULTIPOINT (";
    const auto& coords = points.getCoordinates();
    for (std::size_t i = 0; i < coords.size(); ++i) {
        ss << (i == 0 ? "" : ", ") << "(" << coords[i].x << " " << coords[i].y << ")";
    }
    ss << ")";
    return ss.str();
}

inline json buildGeometryElement(const geometry::MultiLineString& mls, const Projection& projection, bool geoJSON) {
    if (geoJSON) {
        return buildGeometryElement("MultiLineString", buildArray(mls.getLineStrings(), [&](const auto& lineString) {
                                        return buildCoordinatesArray(lineString, projection);
                                    }));
    }
    std::ostringstream ss;
    ss << "MULTILINESTRING (";
    const auto& lineStrings = mls.getLineStrings();
    for (std::size_t i = 0, n = lineStrings.size(); i < n; ++i) {
        if (i != 0) {
            ss << ", ";
        }
        ss << "(";
        const auto& coords = lineStrings[i];
        for (std::size_t j = 0; j < coords.size(); ++j) {
            ss << (j == 0 ? "" : ", ") << coords[j].x << " " << coords[j].y;
        }
        ss << ")";
    }
    ss << ")";
    return ss.str();
}

inline json buildGeometryElement(const geometry::Polygon& poly, const Projection& projection, bool geoJSON) {
    if (geoJSON) {
        return buildGeometryElement("Polygon", buildPolygonCoords(poly.getRings(), projection));
    }
    std::ostringstream ss;
    ss << "POLYGON (";
    const auto& rings = poly.getRings();
    for (std::size_t i = 0, n = rings.size(); i < n; ++i) {
        if (i != 0) {
            ss << ", ";
        }
        ss << "(";
        const auto& coords = rings[i];
        for (std::size_t j = 0; j <= coords.size(); ++j) {
            // Wrap around to the first coordinate to close the ring.
            // See `getLineStringCoords`
            const auto& coord = coords[j % coords.size()];
            ss << (j == 0 ? "" : ", ") << coord.x << " " << coord.y;
        }
        ss << ")";
    }
    ss << ")";
    return ss.str();
}

inline json buildGeometryElement(const geometry::MultiPolygon& poly, const Projection& projection, bool geoJSON) {
    if (geoJSON) {
        return buildGeometryElement("MultiPolygon", buildArray(poly.getPolygons(), [&](const auto& poly) {
                                        return buildPolygonCoords(poly, projection);
                                    }));
    }
    std::ostringstream ss;
    ss << "MULTIPOLYGON (";
    const auto& polygons = poly.getPolygons();
    for (std::size_t i = 0; i < polygons.size(); ++i) {
        if (i != 0) {
            ss << ", ";
        }
        ss << "(";
        const auto& rings = polygons[i];
        for (std::size_t j = 0; j < rings.size(); ++j) {
            if (j != 0) {
                ss << ", ";
            }
            ss << "(";
            const auto& coords = rings[j];
            for (std::size_t k = 0; k <= coords.size(); ++k) {
                const auto& coord = coords[k % coords.size()];
                ss << (k == 0 ? "" : ", ") << coord.x << " " << coord.y;
            }
            ss << ")";
        }
        ss << ")";
    }
    ss << ")";
    return ss.str();
}

inline json buildAnyGeometryElement(const geometry::Geometry& geometry, const Projection& projection, bool geoJSON) {
    switch (geometry.type) {
        case metadata::tileset::GeometryType::POINT:
            return buildGeometryElement(static_cast<const geometry::Point&>(geometry), projection, geoJSON);
        case metadata::tileset::GeometryType::LINESTRING:
            return buildGeometryElement(static_cast<const geometry::LineString&>(geometry), projection, geoJSON);
        case metadata::tileset::GeometryType::POLYGON:
            return buildGeometryElement(static_cast<const geometry::Polygon&>(geometry), projection, geoJSON);
        case metadata::tileset::GeometryType::MULTIPOINT:
            return buildGeometryElement(static_cast<const geometry::MultiPoint&>(geometry), projection, geoJSON);
        case metadata::tileset::GeometryType::MULTILINESTRING:
            return buildGeometryElement(static_cast<const geometry::MultiLineString&>(geometry), projection, geoJSON);
        case metadata::tileset::GeometryType::MULTIPOLYGON:
            return buildGeometryElement(static_cast<const geometry::MultiPolygon&>(geometry), projection, geoJSON);
        default:
            throw std::runtime_error("Unsupported geometry type " + std::to_string(std::to_underlying(geometry.type)));
    }
}
#pragma endregion Geometry

#pragma region Properties
struct PropertyVisitor {
    template <typename T>
    std::optional<json> encodeFloatingPoint(T value, std::string_view prefix) const {
        if (std::isfinite(value)) {
            return json(value);
        }
        if (std::isnan(value)) {
            return json(std::string(prefix) + "::NAN");
        }
        if (std::signbit(value)) {
            return json(std::string(prefix) + "::NEG_INFINITY");
        }
        return json(std::string(prefix) + "::INFINITY");
    }

    std::optional<json> operator()(float value) const { return encodeFloatingPoint(value, "f32"); }
    std::optional<json> operator()(double value) const { return encodeFloatingPoint(value, "f64"); }

    template <typename T>
    std::optional<json> operator()(const T& value) const {
        return value;
    }
};

inline json buildProperties(const Layer& layer, const Feature& feature) {
    auto result = json::object();
    for (const auto& [key, _] : layer.getProperties()) {
        if (const auto property = feature.getProperty(key, layer); property) {
            if (auto json = std::visit(PropertyVisitor(), *property); json) {
                result[key] = std::move(*json);
            }
        }
    }
    return result;
}
#pragma endregion Properties

} // namespace detail

inline json toJSON(const Layer& layer, const Feature& feature, const Projection& projection, bool geoJSON) {
    auto result = json{
        {"geometry", detail::buildAnyGeometryElement(feature.getGeometry(), projection, geoJSON)},
    };
    if (const auto id = feature.getID(); id.has_value()) {
        result["id"] = *id;
    }
    if (geoJSON) {
        result["type"] = "Feature";
    }
    if (!layer.getProperties().empty()) {
        result["properties"] = detail::buildProperties(layer, feature);
    }
    return result;
}

inline json toJSON(const Layer& layer, const TileCoordinate& tileCoord, bool geoJSON) {
    const auto projection = Projection{layer.getExtent(), tileCoord};
    const auto features = std::ranges::views::all(layer.getFeatures());
    return {{"name", layer.getName()},
            {"extent", layer.getExtent()},
            {"features", detail::buildArray(features, [&](const auto& feature) {
                 return toJSON(layer, feature, projection, geoJSON);
             })}};
}

inline json toJSON(const MapLibreTile& tile, const TileCoordinate& tileCoord, bool geoJSON) {
    const auto layers = std::ranges::views::all(tile.getLayers());
    return {
        {"layers", detail::buildArray(layers, [&](const auto& layer) { return toJSON(layer, tileCoord, geoJSON); })},
    };
}

} // namespace mlt::json

#endif
