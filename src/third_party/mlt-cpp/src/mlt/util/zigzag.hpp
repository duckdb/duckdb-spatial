#pragma once

#include <mlt/common.hpp>

#include <type_traits>

namespace mlt::util::decoding {

template <typename T>
    requires(std::is_integral_v<underlying_type_t<T>>)
T decodeZigZag(T encoded) noexcept {
    using UT = underlying_type_t<T>;
    const auto signedValue = static_cast<std::make_signed_t<UT>>(encoded);
    const auto unsignedValue = static_cast<std::make_unsigned_t<UT>>(encoded);
    return static_cast<T>((unsignedValue >> 1) ^ (-(signedValue & 1)));
}

} // namespace mlt::util::decoding
