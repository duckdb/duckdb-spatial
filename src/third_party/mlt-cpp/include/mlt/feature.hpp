#pragma once

#include <mlt/properties.hpp>
#include <mlt/util/noncopyable.hpp>

#include <memory>
#include <optional>

namespace mlt {

namespace geometry {
class Geometry;
} // namespace geometry

class Layer;
class Feature : public util::noncopyable {
public:
    using id_t = std::uint64_t;
    using extent_t = std::uint32_t;
    using Geometry = geometry::Geometry;

    Feature() = delete;
    Feature(Feature&&) noexcept = default;
    Feature& operator=(Feature&&) = delete;

    /// Construct a feature
    /// @param id Feature identifier, or nullopt if the feature has no ID
    /// @param geometry Feature geometry, required
    /// @param index Index of the property in the layer
    Feature(std::optional<id_t> id, std::unique_ptr<Geometry>&&, std::uint32_t index);

    ~Feature() noexcept;

    std::optional<id_t> getID() const noexcept { return ident; }
    std::uint32_t getIndex() const noexcept { return index; }
    const Geometry& getGeometry() const noexcept { return *geometry; }
    std::optional<Property> getProperty(const std::string& key, const Layer&) const;

private:
    std::optional<id_t> ident;
    std::uint32_t index; // index of the property in the layer
    std::unique_ptr<Geometry> geometry;
};

} // namespace mlt
