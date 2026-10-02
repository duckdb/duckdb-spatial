#pragma once

#include <limits>
#include <mlt/coordinate.hpp>

#include <cmath>
#include <stdexcept>

namespace mlt::util {

class SpaceFillingCurve {
public:
    SpaceFillingCurve(std::int32_t minVertexValue, std::int32_t maxVertexValue)
        : coordinateShift(calculateCoordinateShift(minVertexValue)),
          tileExtent(maxVertexValue + coordinateShift),
          numBits(calculateBitsRequired(minVertexValue, maxVertexValue)),
          minBound(minVertexValue),
          maxBound(maxVertexValue) {
        if (numBits > MAX_BITS) {
            throw std::runtime_error("coordinate range is not supported");
        }
        // TODO: fix tile buffer problem
    }
    virtual ~SpaceFillingCurve() = default; // GCOVR_EXCL_LINE

    virtual std::uint32_t encode(const Coordinate& vertex) const = 0;

    virtual Coordinate decode(std::uint32_t mortonCode) const = 0;

    std::uint32_t getNumBits() const noexcept { return numBits; }

    std::int32_t getCoordinateShift() const noexcept { return coordinateShift; }

    static constexpr std::int32_t MAX_BITS = 16;

    static bool validCoordinateRange(std::int32_t minVertexValue, std::int32_t maxVertexValue) {
        return calculateBitsRequired(minVertexValue, maxVertexValue) <= MAX_BITS;
    }

protected:
    static std::int32_t calculateCoordinateShift(std::int32_t minVertexValue) {
        if (minVertexValue == std::numeric_limits<std::int32_t>::min()) {
            return std::numeric_limits<std::int32_t>::max(); // avoid overflow
        }
        return (minVertexValue < 0) ? std::abs(minVertexValue) : 0;
    }
    static std::int32_t calculateExtent(std::int32_t minVertexValue, std::int32_t maxVertexValue) {
        return calculateCoordinateShift(minVertexValue) + maxVertexValue;
    }
    static std::int32_t calculateBitsRequired(std::int32_t extent) {
        return static_cast<std::int32_t>(std::ceil(std::log2(extent + 1)));
    }
    static std::int32_t calculateBitsRequired(std::int32_t minVertexValue, std::int32_t maxVertexValue) {
        return calculateBitsRequired(calculateExtent(minVertexValue, maxVertexValue));
    }

    void validate(const Coordinate& vertex) const {
        // TODO: also check for int overflow as we are limiting the sfc ids to max int size
        if (vertex.x < static_cast<float>(minBound) || vertex.y < static_cast<float>(minBound) ||
            vertex.x > static_cast<float>(maxBound) || vertex.y > static_cast<float>(maxBound)) {
            throw std::runtime_error("The specified tile buffer size is currently not supported");
        }
    }

    const std::int32_t coordinateShift;
    const std::uint32_t tileExtent;
    const std::uint32_t numBits;
    const std::int32_t minBound;
    const std::int32_t maxBound;
};

} // namespace mlt::util
