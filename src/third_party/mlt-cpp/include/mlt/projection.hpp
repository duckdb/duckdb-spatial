#pragma once

#include <mlt/coordinate.hpp>
#include <mlt/layer.hpp>

#include <cmath>
#include <cstdint>
#include <numbers>
#include <stdexcept>

namespace mlt {

// Intended to match the results of
// https://github.com/mapbox/vector-tile-js/blob/77851380b63b07fd0af3d5a3f144cc86fb39fdd1/lib/vectortilefeature.js#L129

class Projection {
public:
    Projection() = delete;
    Projection(const Projection&) noexcept = default;
    Projection(Projection&&) noexcept = default;
    Projection(Layer::extent_t extent, const TileCoordinate& tile)
        : size(extent * (1 << tile.z)),
          x0(extent * tile.x),
          y0(extent * tile.y),
          s1(360.0 / size) {
        if (extent == 0) {
            throw std::runtime_error("Invalid tile extent");
        }
    }

    Coordinate project(const Coordinate& coord) const noexcept { return {projectX(coord.x), projectY(coord.y)}; }

private:
    float projectX(float x) const noexcept { return ((x + x0) * s1) - 180.0f; }
    float projectY(float y) const noexcept {
        return (2.0f * radToDeg(std::atan(std::exp(degToRad(180.0f - ((y + y0) * s1)))))) - 90.0f;
    }

    static float degToRad(float deg) noexcept { return deg * std::numbers::pi / 180.0; }
    static float radToDeg(float rad) noexcept { return rad * 180 / std::numbers::pi; }

    std::uint64_t size;
    std::uint64_t x0;
    std::uint64_t y0;
    double s1;
    constexpr static double s2 = 360.0 / std::numbers::pi;
};

} // namespace mlt
