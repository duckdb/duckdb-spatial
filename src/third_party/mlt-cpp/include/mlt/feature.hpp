#pragma once

#include <mlt/properties.hpp>
#include <mlt/util/noncopyable.hpp>

#include <memory>

namespace mlt {

namespace geometry {
class Geometry;
}
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
    /// @param id Feature identifier
    /// @param geometry Feature geometry, required
    /// @param index Index of the property in the layer
    Feature(id_t id, std::unique_ptr<Geometry>&&, std::uint32_t index);

    ~Feature() noexcept;

    id_t getID() const noexcept { return ident; }
    std::uint32_t getIndex() const noexcept { return index; }
    const Geometry& getGeometry() const noexcept { return *geometry; }
    std::optional<Property> getProperty(const std::string& key, const Layer&) const;

private:
    id_t ident;
    std::uint32_t index; // index of the property in the layer
    std::unique_ptr<Geometry> geometry;
};

} // namespace mlt
