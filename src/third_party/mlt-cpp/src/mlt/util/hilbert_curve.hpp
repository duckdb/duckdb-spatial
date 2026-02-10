#pragma once

#include <mlt/util/space_filling_curve.hpp>

#include <cstdint>
#include <utility>

namespace mlt::util {

/// Hilbert curve using the Skilling algorithm ("Programming the Hilbert curve", 2004),
/// matching the Java `org.davidmoten.hilbert` library used by the MLT reference encoder.
class HilbertCurve : public SpaceFillingCurve {
public:
    HilbertCurve(std::int32_t minVertexValue, std::int32_t maxVertexValue)
        : SpaceFillingCurve(minVertexValue, maxVertexValue) {}

    std::uint32_t encode(const Coordinate& vertex) const override {
        auto x = static_cast<std::uint32_t>(static_cast<std::int32_t>(vertex.x) + coordinateShift);
        auto y = static_cast<std::uint32_t>(static_cast<std::int32_t>(vertex.y) + coordinateShift);
        return xy2d(numBits, x, y);
    }

    Coordinate decode(std::uint32_t hilbertIndex) const noexcept override {
        auto [x, y] = d2xy(numBits, hilbertIndex);
        return {static_cast<float>(static_cast<std::int32_t>(x) - coordinateShift),
                static_cast<float>(static_cast<std::int32_t>(y) - coordinateShift)};
    }

    static std::uint32_t xy2d(std::uint32_t bits, std::uint32_t x, std::uint32_t y) {
        std::uint32_t coords[2] = {x, y};
        axesToTranspose(bits, coords, 2);
        return untranspose(bits, coords, 2);
    }

    static std::pair<std::uint32_t, std::uint32_t> d2xy(std::uint32_t bits, std::uint32_t d) {
        std::uint32_t coords[2];
        transpose(bits, d, coords, 2);
        transposeToAxes(bits, coords, 2);
        return {coords[0], coords[1]};
    }

private:
    static void axesToTranspose(std::uint32_t bits, std::uint32_t* x, int n) {
        auto M = static_cast<std::uint32_t>(1u << (bits - 1));
        for (auto q = M; q > 1; q >>= 1) {
            auto p = q - 1;
            for (int i = 0; i < n; ++i) {
                if (x[i] & q) {
                    x[0] ^= p;
                } else {
                    auto t = (x[0] ^ x[i]) & p;
                    x[0] ^= t;
                    x[i] ^= t;
                }
            }
        }
        for (int i = 1; i < n; ++i) {
            x[i] ^= x[i - 1];
        }
        std::uint32_t t = 0;
        for (auto q = M; q > 1; q >>= 1) {
            if (x[n - 1] & q) {
                t ^= q - 1;
            }
        }
        for (int i = 0; i < n; ++i) {
            x[i] ^= t;
        }
    }

    static void transposeToAxes(std::uint32_t bits, std::uint32_t* x, int n) {
        auto N = static_cast<std::uint32_t>(2u << (bits - 1));
        auto t = x[n - 1] >> 1;
        for (int i = n - 1; i > 0; --i) {
            x[i] ^= x[i - 1];
        }
        x[0] ^= t;
        for (std::uint32_t q = 2; q != N; q <<= 1) {
            auto p = q - 1;
            for (int i = n - 1; i >= 0; --i) {
                if (x[i] & q) {
                    x[0] ^= p;
                } else {
                    auto t2 = (x[0] ^ x[i]) & p;
                    x[0] ^= t2;
                    x[i] ^= t2;
                }
            }
        }
    }

    static std::uint32_t untranspose(std::uint32_t bits, const std::uint32_t* x, int n) {
        std::uint32_t d = 0;
        auto length = bits * n;
        int bIndex = static_cast<int>(length) - 1;
        auto mask = static_cast<std::uint32_t>(1u << (bits - 1));
        for (std::uint32_t i = 0; i < bits; ++i) {
            for (int j = 0; j < n; ++j) {
                if (x[j] & mask) {
                    d |= 1u << bIndex;
                }
                --bIndex;
            }
            mask >>= 1;
        }
        return d;
    }

    static void transpose(std::uint32_t bits, std::uint32_t d, std::uint32_t* x, int n) {
        auto length = bits * n;
        x[0] = 0;
        x[1] = 0;
        for (std::uint32_t idx = 0; idx < length; ++idx) {
            if (d & (1u << idx)) {
                int dim = static_cast<int>(length - idx - 1) % n;
                int shift = static_cast<int>(idx / n) % static_cast<int>(bits);
                x[dim] |= 1u << shift;
            }
        }
    }
};

} // namespace mlt::util
