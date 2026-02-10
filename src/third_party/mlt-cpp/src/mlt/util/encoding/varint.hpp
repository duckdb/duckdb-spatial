#pragma once

#include <cstdint>
#include <type_traits>
#include <vector>

namespace mlt::util::encoding {

template <typename T>
    requires(std::is_integral_v<T> && std::is_unsigned_v<T>)
constexpr std::size_t getVarintSize(T value) noexcept {
    std::size_t size = 1;
    while (value > 0x7f) {
        value >>= 7;
        ++size;
    }
    return size;
}

template <typename T>
    requires(std::is_integral_v<T> && std::is_unsigned_v<T>)
std::size_t encodeVarint(T value, std::uint8_t* out) noexcept {
    std::size_t i = 0;
    while (value > 0x7f) {
        out[i++] = static_cast<std::uint8_t>((value & 0x7f) | 0x80);
        value >>= 7;
    }
    out[i++] = static_cast<std::uint8_t>(value);
    return i;
}

template <typename T>
    requires(std::is_integral_v<T> && std::is_unsigned_v<T>)
void encodeVarint(T value, std::vector<std::uint8_t>& out) {
    std::uint8_t buf[10];
    const auto n = encodeVarint(value, buf);
    out.insert(out.end(), buf, buf + n);
}

template <typename T>
    requires(std::is_integral_v<T>)
void encodeVarints(const T* values, std::size_t count, std::vector<std::uint8_t>& out) {
    for (std::size_t i = 0; i < count; ++i) {
        encodeVarint(static_cast<std::make_unsigned_t<T>>(values[i]), out);
    }
}

} // namespace mlt::util::encoding
