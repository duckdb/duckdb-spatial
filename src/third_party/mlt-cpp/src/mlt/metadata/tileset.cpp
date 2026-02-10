#include <mlt/metadata/tileset.hpp>
#include <mlt/metadata/type_map.hpp>
#include <mlt/util/buffer_stream.hpp>
#include <mlt/util/encoding/varint.hpp>
#include <mlt/util/stl.hpp>
#include <mlt/util/varint.hpp>

#include <utility>

namespace mlt::metadata::tileset {
using util::decoding::decodeVarint;

namespace {
std::string decodeString(BufferStream& tileData) {
    std::string result;
    const auto length = decodeVarint<std::uint32_t>(tileData);
    if (length > 0) {
        result.resize(length);
        tileData.read(result.data(), length);
    }
    return result;
}

Column decodeColumn(BufferStream& tileData) {
    const auto typeCode = decodeVarint<std::uint32_t>(tileData);
    auto column = type_map::Tag0x01::decodeColumnType(typeCode);
    if (!column) {
        // We can't just skip this because we don't know the actual length
        throw std::runtime_error("Unsupported column type code: " + std::to_string(typeCode));
    }

    if (type_map::Tag0x01::columnTypeHasName(typeCode)) {
        column->name = decodeString(tileData);
    }

    if (type_map::Tag0x01::columnTypeHasChildren(typeCode)) {
        assert(column->hasComplexType());
        auto& complex = column->getComplexType();
        const auto childCount = decodeVarint<std::uint32_t>(tileData);
        complex.children = util::generateVector<Column>(childCount, [&](auto) { return decodeColumn(tileData); });
    }

    return *column;
}
} // namespace

FeatureTable decodeFeatureTable(BufferStream& tileData) {
    auto name = decodeString(tileData);
    const auto extent = decodeVarint<std::uint32_t>(tileData);
    const auto columnCount = decodeVarint<std::uint32_t>(tileData);
    auto columns = util::generateVector<Column>(columnCount, [&](auto) { return decodeColumn(tileData); });
    return {.name = std::move(name), .extent = extent, .columns = std::move(columns)};
}

namespace {
void encodeString(const std::string& str, std::vector<std::uint8_t>& out) {
    util::encoding::encodeVarint(static_cast<std::uint32_t>(str.size()), out);
    out.insert(out.end(), str.begin(), str.end());
}

void encodeColumn(const Column& column, std::vector<std::uint8_t>& out) {
    const bool hasChildren = column.hasComplexType() && column.getComplexType().hasChildren();
    const auto typeCode = std::visit(util::overloaded{
                                         [&](const ScalarColumn& scalar) -> std::optional<std::uint32_t> {
                                             if (scalar.hasPhysicalType()) {
                                                 return type_map::Tag0x01::encodeColumnType(scalar.getPhysicalType(),
                                                                                            std::nullopt,
                                                                                            std::nullopt,
                                                                                            std::nullopt,
                                                                                            column.nullable,
                                                                                            hasChildren,
                                                                                            scalar.hasLongID);
                                             }
                                             return type_map::Tag0x01::encodeColumnType(std::nullopt,
                                                                                        scalar.getLogicalType(),
                                                                                        std::nullopt,
                                                                                        std::nullopt,
                                                                                        column.nullable,
                                                                                        hasChildren,
                                                                                        scalar.hasLongID);
                                         },
                                         [&](const ComplexColumn& complex) -> std::optional<std::uint32_t> {
                                             if (complex.hasPhysicalType()) {
                                                 return type_map::Tag0x01::encodeColumnType(std::nullopt,
                                                                                            std::nullopt,
                                                                                            complex.getPhysicalType(),
                                                                                            std::nullopt,
                                                                                            column.nullable,
                                                                                            hasChildren,
                                                                                            false);
                                             }
                                             return type_map::Tag0x01::encodeColumnType(std::nullopt,
                                                                                        std::nullopt,
                                                                                        std::nullopt,
                                                                                        complex.getLogicalType(),
                                                                                        column.nullable,
                                                                                        hasChildren,
                                                                                        false);
                                         },
                                     },
                                     column.type);

    if (!typeCode) {
        throw std::runtime_error("Cannot encode column type for: " + column.name);
    }
    util::encoding::encodeVarint(*typeCode, out);

    if (type_map::Tag0x01::columnTypeHasName(*typeCode)) {
        encodeString(column.name, out);
    }

    if (type_map::Tag0x01::columnTypeHasChildren(*typeCode)) {
        const auto& children = column.getComplexType().children;
        util::encoding::encodeVarint(static_cast<std::uint32_t>(children.size()), out);
        for (const auto& child : children) {
            encodeColumn(child, out);
        }
    }
}
} // namespace

std::vector<std::uint8_t> encodeFeatureTable(const FeatureTable& table) {
    std::vector<std::uint8_t> result;
    result.reserve(256);

    encodeString(table.name, result);
    util::encoding::encodeVarint(table.extent, result);
    util::encoding::encodeVarint(static_cast<std::uint32_t>(table.columns.size()), result);
    for (const auto& column : table.columns) {
        encodeColumn(column, result);
    }
    return result;
}

} // namespace mlt::metadata::tileset
