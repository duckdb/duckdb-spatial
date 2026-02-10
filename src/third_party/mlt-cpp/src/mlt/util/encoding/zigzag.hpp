#pragma once

#include <mlt/common.hpp>

#include <type_traits>

namespace mlt::util::encoding {

template <typename T>
    requires(std::is_integral_v<underlying_type_t<T>>)
auto encodeZigZag(T value) noexcept {
    using UT = underlying_type_t<T>;
    constexpr auto bits = sizeof(UT) * 8 - 1;
    const auto signedValue = static_cast<std::make_signed_t<UT>>(value);
    return static_cast<std::make_unsigned_t<UT>>((signedValue << 1) ^ (signedValue >> bits));
}

} // namespace mlt::util::encoding
