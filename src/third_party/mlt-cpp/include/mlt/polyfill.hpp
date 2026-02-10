#pragma once

#include <algorithm>
#include <bit>
#include <ranges> // IWYU pragma: keep - Needed by MSVC
#include <utility>

namespace std {
#if !__has_cpp_attribute(__cpp_lib_to_underlying)
template <typename E>
constexpr auto to_underlying(E e) noexcept {
    return static_cast<std::underlying_type_t<E>>(e);
}
#endif

#if !__has_cpp_attribute(__cpp_lib_byteswap)
template <std::integral T>
constexpr T byteswap(T value) noexcept {
    static_assert(std::has_unique_object_representations_v<T>, "T may not have padding bits");
    auto value_representation = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
    std::ranges::reverse(value_representation);
    return std::bit_cast<T>(value_representation);
}
#endif
} // namespace std
