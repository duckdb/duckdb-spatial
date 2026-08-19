#pragma once

#include <mlt/util/space_filling_curve.hpp>

namespace mlt::util {

class MortonCurve : public SpaceFillingCurve {
public:
    MortonCurve(std::int32_t minVertexValue, std::int32_t maxVertexValue)
        : SpaceFillingCurve(minVertexValue, maxVertexValue) {}

    std::uint32_t encode(const Coordinate& vertex) const override {
        validate(vertex);
        return encodeMorton(vertex, numBits, coordinateShift);
    }

    Coordinate decode(std::uint32_t mortonCode) const noexcept override {
        return decode(mortonCode, numBits, coordinateShift);
    }

    static Coordinate decode(std::uint32_t mortonCode, std::uint32_t numBits, std::int32_t coordinateShift) noexcept {
        return {static_cast<float>(decode(mortonCode, numBits) - coordinateShift),
                static_cast<float>(decode(mortonCode >> 1, numBits) - coordinateShift)};
    }

    static std::int32_t decode(std::uint32_t code, std::uint32_t numBits) noexcept {
        std::uint32_t coordinate = 0;
        for (std::uint32_t i = 0; i < numBits; ++i) {
            coordinate |= (code & (1ul << (2 * i))) >> i;
        }
        return static_cast<std::int32_t>(coordinate);
    }

private:
    static std::uint32_t encodeMorton(const Coordinate& vertex,
                                      std::uint32_t numBits,
                                      std::int32_t coordinateShift) noexcept {
        const auto shiftedX = static_cast<std::uint32_t>(vertex.x + coordinateShift);
        const auto shiftedY = static_cast<std::uint32_t>(vertex.y + coordinateShift);
        std::uint32_t mortonCode = 0;
        for (std::uint32_t i = 0; i < numBits; ++i) {
            const auto mask = 1u << i;
            mortonCode |= ((shiftedX & mask) << i) | ((shiftedY & mask) << (i + 1));
        }
        return mortonCode;
    }
};

} // namespace mlt::util
