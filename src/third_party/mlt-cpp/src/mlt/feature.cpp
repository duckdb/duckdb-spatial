#include <mlt/feature.hpp>

#include <mlt/geometry.hpp> // NOLINT(misc-include-cleaner)
#include <mlt/layer.hpp>
#include <mlt/properties.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace mlt {

Feature::Feature(std::optional<id_t> ident_, std::unique_ptr<Geometry>&& geometry_, std::uint32_t index_)
    : ident(ident_),
      index(index_),
      geometry(std::move(geometry_)) {}

Feature::~Feature() noexcept = default;

std::optional<Property> Feature::getProperty(const std::string& key, const Layer& layer) const {
    const auto& propertyMap = layer.getProperties();
    if (const auto hit = propertyMap.find(key); hit != propertyMap.end()) {
        return hit->second.getProperty(index);
    }
    return std::nullopt;
}

} // namespace mlt
