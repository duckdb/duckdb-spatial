#pragma once

#include <mlt/coordinate.hpp>

#include <cmath>
#include <stdexcept>

namespace mlt::util {

class SpaceFillingCurve {
public:
    SpaceFillingCurve(std::int32_t minVertexValue, std::int32_t maxVertexValue)
        : coordinateShift((minVertexValue < 0) ? std::abs(minVertexValue) : 0),
          tileExtent(maxVertexValue + coordinateShift),
          numBits(static_cast<std::uint32_t>(std::ceil(std::log2(tileExtent + 1)))),
          minBound(minVertexValue),
          maxBound(maxVertexValue) {
        // TODO: fix tile buffer problem
    }
    virtual ~SpaceFillingCurve() = default;

    virtual std::uint32_t encode(const Coordinate& vertex) const = 0;

    virtual Coordinate decode(std::uint32_t mortonCode) const = 0;

    std::uint32_t getNumBits() const noexcept { return numBits; }

    std::int32_t getCoordinateShift() const noexcept { return coordinateShift; }

protected:
    void validate(const Coordinate& vertex) const {
        // TODO: also check for int overflow as we are limiting the sfc ids to max int size
        if (vertex.x < static_cast<float>(minBound) || vertex.y < static_cast<float>(minBound) ||
            vertex.x > static_cast<float>(maxBound) || vertex.y > static_cast<float>(maxBound)) {
            throw std::runtime_error("The specified tile buffer size is currently not supported");
        }
    }

protected:
    const std::int32_t coordinateShift;
    const std::uint32_t tileExtent;
    const std::uint32_t numBits;
    const std::int32_t minBound;
    const std::int32_t maxBound;
};

} // namespace mlt::util
