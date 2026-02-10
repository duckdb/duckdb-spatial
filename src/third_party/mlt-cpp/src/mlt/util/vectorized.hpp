#pragma once

#include <mlt/common.hpp>

#include <cassert>
#include <type_traits>

namespace mlt::util::decoding::vectorized {
namespace {
/// JS/Java `>>>`
template <int bits, typename T>
    requires(std::integral<T>)
T unsigned_rshift(T t) {
    return static_cast<T>(static_cast<std::make_unsigned_t<T>>(t) >> bits);
}

/// Shifts the ones bit into the sign, for reasons
template <typename T>
    requires(std::integral<T>)
T odd_sign(T t) {
    return (t << 31) >> 31;
}

template <typename T>
    requires(std::integral<T>)
T shift_and_xor(T t) {
    return unsigned_rshift<1>(t) ^ odd_sign(t);
}

} // namespace

template <typename T>
    requires(std::is_integral_v<T> && sizeof(T) == 4)
inline void decodeComponentwiseDeltaVec2(T* const data, const std::size_t count) noexcept {
    assert((count % 2) == 0);
    if (1 < count) {
        data[0] = shift_and_xor(data[0]);
        data[1] = shift_and_xor(data[1]);
        for (std::size_t i = 2; i + 1 < count; i += 2) {
            data[i] = shift_and_xor(data[i]) + data[i - 2];
            data[i + 1] = shift_and_xor(data[i + 1]) + data[i - 1];
        }
    }
}

} // namespace mlt::util::decoding::vectorized
