#pragma once

#include <mlt/common.hpp>
#include <mlt/metadata/stream.hpp>
#include <mlt/metadata/tileset.hpp>
#include <mlt/util/noncopyable.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace mlt {

struct EncoderConfig {
    bool useFastPfor = false;
    bool includeIds = true;
    bool sortFeatures = true;
    bool preTessellate = false;
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
        std::uint64_t id = 0;
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

    std::vector<std::uint8_t> encode(const std::vector<Layer>& layers, const EncoderConfig& config = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace mlt
