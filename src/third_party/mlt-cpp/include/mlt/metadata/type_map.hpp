#include <cassert>
#include <cstddef>
#include <optional>

#include <mlt/metadata/tileset.hpp>

namespace mlt::metadata::type_map {
struct Tag0x01 {
    using Column = metadata::tileset::Column;
    using ScalarColumn = metadata::tileset::ScalarColumn;
    using ComplexColumn = metadata::tileset::ComplexColumn;
    using ScalarType = metadata::tileset::ScalarType;
    using ComplexType = metadata::tileset::ComplexType;
    using LogicalScalarType = metadata::tileset::LogicalScalarType;
    using LogicalComplexType = metadata::tileset::LogicalComplexType;

    /// Produces the unique type encoding for a `Column` or `Field`
    static std::optional<std::uint32_t> encodeColumnType(std::optional<ScalarType> physicalScalarType,
                                                         std::optional<LogicalScalarType> logicalScalarType,
                                                         std::optional<ComplexType> physicalComplexType,
                                                         std::optional<LogicalComplexType>,
                                                         bool isNullable,
                                                         bool hasChildren,
                                                         bool hasLongIDs) {
        if (physicalScalarType) {
            if (!hasChildren) {
                return mapScalarType(*physicalScalarType, isNullable);
            }
        } else if (logicalScalarType) {
            if (logicalScalarType == LogicalScalarType::ID) {
                return (hasLongIDs ? 2 : 0) | (isNullable ? 1 : 0);
            }
        } else if (physicalComplexType) {
            if (physicalComplexType == ComplexType::GEOMETRY) {
                if (!isNullable && !hasChildren) {
                    return 4;
                }
            } else if (physicalComplexType == ComplexType::STRUCT) {
                if (!isNullable && hasChildren) {
                    return 30;
                }
            }
        }
        return std::nullopt;
    }

    /// Re-create a `Column` from the unique type code.
    /// The inverse of `encodeColumnType`
    static std::optional<Column> decodeColumnType(std::uint32_t typeCode) {
        using ColumnScope = metadata::tileset::ColumnScope;
        switch (typeCode) {
            case 0:
            case 1:
            case 2:
            case 3:
                return Column{.name = {},
                              .nullable = (typeCode & 1) != 0,
                              .columnScope = ColumnScope::FEATURE,
                              .type = ScalarColumn{.type = LogicalScalarType::ID, .hasLongID = typeCode > 1}};
            case 4:
                return Column{.name = {},
                              .nullable = false,
                              .columnScope = ColumnScope::FEATURE,
                              .type = ComplexColumn{.type = ComplexType::GEOMETRY}};
            case 30:
                return Column{.name = {},
                              .nullable = false,
                              .columnScope = ColumnScope::FEATURE,
                              .type = ComplexColumn{.type = ComplexType::STRUCT}};
            default:
                if (const auto type = mapScalarType(typeCode); type) {
                    return Column{.name = {},
                                  .nullable = (typeCode & 1) != 0,
                                  .columnScope = ColumnScope::FEATURE,
                                  .type = ScalarColumn{.type = *type}};
                }
                return std::nullopt;
        }
    }

    static bool columnTypeHasName(std::uint32_t typeCode) { return (10 <= typeCode); }

    static bool columnTypeHasChildren(std::uint32_t typeCode) { return (typeCode == 30); }

    static bool hasStreamCount(const Column& column) {
        if (column.hasScalarType()) {
            if (column.getScalarType().hasPhysicalType()) {
                switch (column.getScalarType().getPhysicalType()) {
                    default:
                    case ScalarType::BOOLEAN:
                    case ScalarType::INT_8:
                    case ScalarType::UINT_8:
                    case ScalarType::INT_32:
                    case ScalarType::UINT_32:
                    case ScalarType::INT_64:
                    case ScalarType::UINT_64:
                    case ScalarType::FLOAT:
                    case ScalarType::DOUBLE:
                        return false;
                    case ScalarType::STRING:
                        return true;
                };
            } else if (column.getScalarType().hasLogicalType()) {
                if (column.getScalarType().getLogicalType() == LogicalScalarType::ID) {
                    return false;
                }
            }
        } else if (column.hasComplexType()) {
            if (column.getComplexType().hasPhysicalType()) {
                switch (column.getComplexType().getPhysicalType()) {
                    case ComplexType::GEOMETRY:
                    case ComplexType::STRUCT:
                        return true;
                    default:
                        break;
                }
            }
        }
        // other cases should be impossible
        assert(false);
        return false;
    }

private:
    constexpr static std::optional<ScalarType> mapScalarType(std::uint32_t typeCode) {
        switch (typeCode) {
            case 10:
            case 11:
                return ScalarType::BOOLEAN;
            case 12:
            case 13:
                return ScalarType::INT_8;
            case 14:
            case 15:
                return ScalarType::UINT_8;
            case 16:
            case 17:
                return ScalarType::INT_32;
            case 18:
            case 19:
                return ScalarType::UINT_32;
            case 20:
            case 21:
                return ScalarType::INT_64;
            case 22:
            case 23:
                return ScalarType::UINT_64;
            case 24:
            case 25:
                return ScalarType::FLOAT;
            case 26:
            case 27:
                return ScalarType::DOUBLE;
            case 28:
            case 29:
                return ScalarType::STRING;
            default:
                return std::nullopt;
        }
    }
    constexpr static std::optional<std::uint32_t> mapScalarType(ScalarType typeCode, bool isNullable) {
        switch (typeCode) {
            case ScalarType::BOOLEAN:
                return isNullable ? 11 : 10;
            case ScalarType::INT_8:
                return isNullable ? 13 : 12;
            case ScalarType::UINT_8:
                return isNullable ? 15 : 14;
            case ScalarType::INT_32:
                return isNullable ? 17 : 16;
            case ScalarType::UINT_32:
                return isNullable ? 19 : 18;
            case ScalarType::INT_64:
                return isNullable ? 21 : 20;
            case ScalarType::UINT_64:
                return isNullable ? 23 : 22;
            case ScalarType::FLOAT:
                return isNullable ? 25 : 24;
            case ScalarType::DOUBLE:
                return isNullable ? 27 : 26;
            case ScalarType::STRING:
                return isNullable ? 29 : 28;
            default:
                assert(false);
                return std::nullopt;
        }
    }
};
} // namespace mlt::metadata::type_map
