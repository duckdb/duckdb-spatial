#pragma once

#include <mlt/common.hpp>
#include <mlt/metadata/stream.hpp>
#include <mlt/metadata/tileset.hpp>
#include <mlt/util/noncopyable.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace mlt {

enum class IntegerEncodingOption : std::uint8_t {
    AUTO = 0,
    PLAIN = 1,
    DELTA = 2,
    RLE = 3,
    DELTA_RLE = 4,
};

struct EncoderConfig {
    bool useFastPfor = false;
    bool includeIds = true;
    bool sortFeatures = true;
    bool preTessellate = false;
    bool includeOutlines = true;
    bool enableMortonEncoding = true;
    bool useFsst = true;

    // The following options are primarily for testing, and not expected to be useful in production.

    /// Force the use of nullable columns for properties, even if all features have a value for that property.
    bool forceNullableColumns = false;
    /// The strategy for encoding integer streams, including integer properties and string lengths
    IntegerEncodingOption integerEncodingOption = IntegerEncodingOption::AUTO;
    /// The strategy for encoding integer streams for geometry coordinates and indexes
    std::optional<IntegerEncodingOption> geometryEncodingOption = IntegerEncodingOption::AUTO;
    /// The strategy for encoding geometry topology integer streams (e.g. part sizes)
    /// `geometryEncodingOption` is used as a fallback if this is not set.
    std::optional<IntegerEncodingOption> geometryTopologyEncodingOption = std::nullopt;

    EncoderConfig update(const std::function<void(EncoderConfig&)>& configurator) {
        EncoderConfig config = *this;
        configurator(config);
        return config;
    }

    static EncoderConfig with(const std::function<void(EncoderConfig&)>& configurator) {
        return EncoderConfig().update(configurator);
    }
};

class Encoder : public util::noncopyable {
public:
    using GeometryType = metadata::tileset::GeometryType;

    struct Vertex {
        std::int32_t x;
        std::int32_t y;
    };

    using StructValue = std::map<std::string, std::string>;

    using PropertyValue = std::variant<bool,
                                       std::int32_t,
                                       std::int64_t,
                                       std::uint32_t,
                                       std::uint64_t,
                                       float,
                                       double,
                                       std::string,
                                       StructValue>;

    struct Geometry {
        GeometryType type;
        std::vector<Vertex> coordinates;
        std::vector<std::vector<Vertex>> parts;
        std::vector<std::uint32_t> ringSizes;
        std::vector<std::vector<std::uint32_t>> partRingSizes;
    };

    struct Feature {
        std::optional<std::uint64_t> id = 0;
        Geometry geometry;
        std::map<std::string, PropertyValue> properties;
    };

    struct Layer {
        std::string name;
        std::uint32_t extent = 4096;
        std::vector<Feature> features;
    };

    Encoder();
    ~Encoder() noexcept;

    Encoder(Encoder&&) = delete;
    Encoder& operator=(Encoder&&) = delete;

    std::vector<std::uint8_t> encode(const std::vector<Layer>& layers, const EncoderConfig& config = {}) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace mlt
