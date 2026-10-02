#include <mlt/decoder.hpp>

#include <mlt/common.hpp>
#include <mlt/decode/geometry.hpp>
#include <mlt/decode/int.hpp>
#include <mlt/decode/property.hpp>
#include <mlt/decode/string.hpp>
#include <mlt/feature.hpp>
#include <mlt/geometry_vector.hpp>
#include <mlt/layer.hpp>
#include <mlt/metadata/stream.hpp>
#include <mlt/metadata/tileset.hpp>
#include <mlt/metadata/type_map.hpp>
#include <mlt/properties.hpp>
#include <mlt/tile.hpp>
#include <mlt/util/buffer_stream.hpp>
#include <mlt/util/packed_bitset.hpp>
#include <mlt/util/rle.hpp>
#include <mlt/util/stl.hpp>
#include <mlt/util/varint.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mlt {

using namespace decoder;
using namespace util::decoding;

struct Decoder::Impl {
    Impl(std::unique_ptr<GeometryFactory>&& geometryFactory_, bool supportFastPFOR)
        : integerDecoder(supportFastPFOR),
          geometryDecoder(integerDecoder),
          geometryFactory(std::move(geometryFactory_)) {}

    Layer parseBasicMVTEquivalent(BufferStream&);
    static std::vector<Feature> makeFeatures(const std::vector<std::optional<Feature::id_t>>&,
                                             std::vector<std::unique_ptr<Geometry>>&&);

    IntegerDecoder integerDecoder;
    StringDecoder stringDecoder{integerDecoder};
    PropertyDecoder propertyDecoder{integerDecoder, stringDecoder};
    GeometryDecoder geometryDecoder;
    std::unique_ptr<GeometryFactory> geometryFactory;
};

Layer Decoder::Impl::parseBasicMVTEquivalent(BufferStream& tileData) {
    using mlt::metadata::stream::StreamMetadata;
    using mlt::metadata::type_map::Tag0x01;

    const auto layerMetadata = mlt::metadata::tileset::decodeFeatureTable(tileData);

    std::vector<std::optional<Feature::id_t>> ids;
    std::unique_ptr<geometry::GeometryVector> geometryVector;
    PropertyVecMap properties;

    for (const auto& columnMetadata : layerMetadata.columns) {
        const auto numStreams = Tag0x01::hasStreamCount(columnMetadata) ? decodeVarint<std::uint32_t>(tileData) : 1;
        if (columnMetadata.isID()) {
            PackedBitset idPresentBits;
            std::uint32_t numFeaturesFromPresent = 0;
            if (columnMetadata.nullable) {
                const auto presentStreamMetadata = StreamMetadata::decode(tileData);
                numFeaturesFromPresent = presentStreamMetadata->getNumValues();
                rle::decodeBoolean(tileData, idPresentBits, *presentStreamMetadata, /*consume=*/true);
            }

            const auto idDataStreamMetadata = StreamMetadata::decode(tileData);

            std::vector<Feature::id_t> denseIds(idDataStreamMetadata->getNumValues());
            if (columnMetadata.getScalarType().hasLongID) {
                integerDecoder.decodeIntStream<std::uint64_t>(tileData, denseIds, *idDataStreamMetadata);
            } else {
                // Decode 32-bit ids at 32-bit width so delta math wraps there, then widen to id_t.
                integerDecoder.decodeIntStream<std::uint32_t, std::uint32_t>(tileData, denseIds, *idDataStreamMetadata);
            }

            if (!idPresentBits.empty()) {
                // Expand dense IDs to sparse optional IDs using the present bitset
                ids.resize(numFeaturesFromPresent);
                std::size_t denseIdx = 0;
                for (std::uint32_t i = 0; i < numFeaturesFromPresent; ++i) {
                    if (testBit(idPresentBits, i)) {
                        ids[i] = denseIds[denseIdx++];
                    }
                }
            } else {
                ids.resize(denseIds.size());
                for (std::size_t i = 0; i < denseIds.size(); ++i) {
                    ids[i] = denseIds[i];
                }
            }
        } else if (columnMetadata.isGeometry()) {
            geometryVector = geometryDecoder.decodeGeometryColumn(tileData, columnMetadata, numStreams);
        } else {
            // Decode the property column, which may result in multiple properties (e.g. struct), and merge the results
            auto columnProperties = propertyDecoder.decodePropertyColumn(tileData, columnMetadata, numStreams);
            std::copy(std::make_move_iterator(columnProperties.begin()),
                      std::make_move_iterator(columnProperties.end()),
                      std::inserter(properties, properties.end()));
        }
    }

    // Check framing, we expect to use all the data in the buffer provided.
    if (tileData.available() > 0) {
        throw std::runtime_error(std::to_string(tileData.available()) + " bytes trailing layer " + layerMetadata.name);
    }

    return {layerMetadata.name,
            layerMetadata.extent,
            std::move(geometryVector),
            makeFeatures(ids, geometryVector->getGeometries(*geometryFactory)),
            std::move(properties)};
}

std::vector<Feature> Decoder::Impl::makeFeatures(const std::vector<std::optional<Feature::id_t>>& ids,
                                                 std::vector<std::unique_ptr<Geometry>>&& geometries) {
    const auto featureCount = geometries.size();
    if (!ids.empty() && ids.size() != featureCount) {
        throw std::runtime_error("ID count (" + std::to_string(ids.size()) + ") does not match geometry count (" +
                                 std::to_string(featureCount) + ")");
    }

    return util::generateVector<Feature>(featureCount, [&](const auto i) {
        const auto id = ids.empty() ? std::nullopt : ids[i];
        return Feature{id, std::move(geometries[i]), static_cast<std::uint32_t>(i)};
    });
}

Decoder::Decoder(bool supportFastPFOR)
    : Decoder(std::make_unique<GeometryFactory>(), supportFastPFOR) {}

Decoder::Decoder(std::unique_ptr<GeometryFactory>&& geometryFactory, bool supportFastPFOR)
    : impl{std::make_unique<Impl>(std::move(geometryFactory), supportFastPFOR)} {}

Decoder::~Decoder() noexcept = default;

MapLibreTile Decoder::decode(BufferStream tileData) {
    std::vector<Layer> layers;
    while (tileData.available()) {
        const auto layerLength = decodeVarint<std::uint32_t>(tileData);

        // Create a new BufferStream for this layer and advance past it in the main stream
        auto layerStream = tileData.getSubStream(0, layerLength);
        tileData.consume(layerLength);

        const auto layerTag = decodeVarint<std::uint32_t>(layerStream);
        if (layerTag == 1) {
            layers.push_back(impl->parseBasicMVTEquivalent(layerStream));
        } else {
            // Skipping unknown layer
        }
    }
    return {std::move(layers)};
}

MapLibreTile Decoder::decode(DataView tileData) {
    return decode(BufferStream{tileData});
}

} // namespace mlt
