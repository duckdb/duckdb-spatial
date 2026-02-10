#pragma once

#include <mlt/polyfill.hpp>

#include <string_view>
#include <type_traits>

namespace mlt {

using DataView = std::string_view;

template <typename T, std::size_t N>
constexpr std::size_t countof(T (&)[N]) {
    return N;
}

/// `std::underlying_type` that doesn't fail when given a simple type
template <typename T, bool = std::is_enum_v<T>>
struct underlying_type {
    using type = T;
};
template <typename T>
struct underlying_type<T, true> : ::std::underlying_type<T> {};
template <class T>
using underlying_type_t = typename underlying_type<T>::type;

} // namespace mlt
