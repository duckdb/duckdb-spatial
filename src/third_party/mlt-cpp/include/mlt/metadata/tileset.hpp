#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace mlt {
struct BufferStream;
}

namespace mlt::metadata::tileset {

// See https://maplibre.org/maplibre-tile-spec/specification/
namespace schema {

enum class ColumnScope {
    // 1:1 Mapping of property and feature -> id and geometry
    FEATURE = 0,
    // For M-Values -> 1:1 Mapping for property and vertex
    VERTEX = 1,
};

enum class ScalarType {
    BOOLEAN = 0,
    INT_8 = 1,
    UINT_8 = 2,
    INT_32 = 3,
    UINT_32 = 4,
    INT_64 = 5,
    UINT_64 = 6,
    FLOAT = 7,
    DOUBLE = 8,
    STRING = 9,
};

enum class ComplexType {
    // vec2<Int32> for the VertexBuffer stream with additional information
    // (streams) about the topology
    GEOMETRY = 0,
    // vec3<Int32> for the VertexBuffer stream with additional information
    // (streams) about the topology
    STRUCT = 1,
};

enum class LogicalScalarType {
    // uin32 or 64_t depending on hasLongID
    ID = 0,
};

enum class LogicalComplexType {
};

} // namespace schema

using schema::ColumnScope;
using schema::ComplexType;
using schema::LogicalComplexType;
using schema::LogicalScalarType;
using schema::ScalarType;

enum class GeometryType {
    POINT = 0,
    LINESTRING = 1,
    POLYGON = 2,
    MULTIPOINT = 3,
    MULTILINESTRING = 4,
    MULTIPOLYGON = 5,
};

struct ScalarColumn {
    std::variant<ScalarType, LogicalScalarType> type;
    bool hasLongID = false;

    bool hasPhysicalType() const { return std::holds_alternative<ScalarType>(type); }
    bool hasLogicalType() const { return std::holds_alternative<LogicalScalarType>(type); }
    auto& getPhysicalType() { return std::get<ScalarType>(type); }
    auto& getPhysicalType() const { return std::get<ScalarType>(type); }
    auto& getLogicalType() { return std::get<LogicalScalarType>(type); }
    auto& getLogicalType() const { return std::get<LogicalScalarType>(type); }
    bool isID() const { return hasLogicalType() && getLogicalType() == LogicalScalarType::ID; }
};

// The type tree is flattened in to a list via a pre-order traversal.
// Represents a column if it is a root (top-level) type or a child of a nested type.
struct Column;

struct ComplexColumn {
    std::variant<ComplexType, LogicalComplexType> type;

    // The complex type Geometry and the logical type BINARY have no children
    // since there layout is implicit known. RangeMap has only one child
    // specifying the type of the value since the key is always a vec2<double>.
    std::vector<Column> children;

    bool hasChildren() const { return !children.empty(); }
    bool hasPhysicalType() const { return std::holds_alternative<ComplexType>(type); }
    bool hasLogicalType() const { return std::holds_alternative<LogicalComplexType>(type); }
    auto& getPhysicalType() { return std::get<ComplexType>(type); }
    auto& getPhysicalType() const { return std::get<ComplexType>(type); }
    auto& getLogicalType() { return std::get<LogicalComplexType>(type); }
    auto& getLogicalType() const { return std::get<LogicalComplexType>(type); }
    bool isGeometry() const { return hasPhysicalType() && getPhysicalType() == ComplexType::GEOMETRY; }
    bool isStruct() const { return hasPhysicalType() && getPhysicalType() == ComplexType::STRUCT; }
};

// Column are top-level types in the schema
struct Column {
    std::string name;
    bool nullable = false;
    ColumnScope columnScope = ColumnScope::FEATURE;
    std::variant<ScalarColumn, ComplexColumn> type;

    bool hasScalarType() const { return std::holds_alternative<ScalarColumn>(type); }
    bool hasComplexType() const { return std::holds_alternative<ComplexColumn>(type); }
    auto& getScalarType() { return std::get<ScalarColumn>(type); }
    auto& getScalarType() const { return std::get<ScalarColumn>(type); }
    auto& getComplexType() { return std::get<ComplexColumn>(type); }
    auto& getComplexType() const { return std::get<ComplexColumn>(type); }
    bool isID() const { return hasScalarType() && getScalarType().isID(); }
    bool isGeometry() const { return hasComplexType() && getComplexType().isGeometry(); }
    bool isStruct() const { return hasComplexType() && getComplexType().isStruct(); }
};

struct FeatureTable {
    std::string name;
    std::uint32_t extent;
    std::vector<Column> columns;
};

FeatureTable decodeFeatureTable(BufferStream&);

/// Encode a feature table header into binary format (inverse of decodeFeatureTable).
std::vector<std::uint8_t> encodeFeatureTable(const FeatureTable&);

} // namespace mlt::metadata::tileset
