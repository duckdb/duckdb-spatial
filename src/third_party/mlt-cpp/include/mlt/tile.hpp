#pragma once

#include <mlt/layer.hpp>
#include <mlt/util/noncopyable.hpp>

#include <vector>

namespace mlt {

class MapLibreTile : public util::noncopyable {
public:
    MapLibreTile() = delete;
    MapLibreTile(std::vector<Layer> layers_) noexcept
        : layers(std::move(layers_)) {}
    MapLibreTile(MapLibreTile&&) noexcept = default;
    MapLibreTile& operator=(MapLibreTile&&) noexcept = default;

    const std::vector<Layer>& getLayers() const noexcept { return layers; }

    const Layer* getLayer(const std::string_view& name) const noexcept {
        auto hit = std::ranges::find_if(layers, [&](const auto& layer) { return layer.getName() == name; });
        return (hit != layers.end()) ? &*hit : nullptr;
    }

private:
    std::vector<Layer> layers;
};

} // namespace mlt
