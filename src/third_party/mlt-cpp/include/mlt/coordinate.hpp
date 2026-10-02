#pragma once

#include <cstdint>
#include <vector>

namespace mlt {

struct Coordinate {
    float x;
    float y;

    Coordinate(float x_, float y_) noexcept
        : x(x_),
          y(y_) {}

    bool operator==(const Coordinate& other) const noexcept { return x == other.x && y == other.y; }
    bool operator!=(const Coordinate& other) const noexcept { return !(*this == other); }
};
using CoordVec = std::vector<Coordinate>;

struct TileCoordinate {
    std::uint32_t x;
    std::uint32_t y;
    std::uint32_t z;
};

} // namespace mlt
