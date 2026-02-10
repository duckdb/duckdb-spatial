#pragma once

#include <mlt/common.hpp>
#include <mlt/util/buffer_stream.hpp>

#include <stdexcept>
#include <type_traits>

namespace mlt::util::decoding {

/// Returns the size of the varint encoding for a given value.
/// Equivalent to `max(1,ceil(log128(value)))`
template <typename T = std::uint32_t>
std::size_t getVarintSize(T value) {
    return std::max<std::size_t>(1, ((8 * sizeof(value)) - std::countl_zero(value) + 6) / 7);
}

template <typename T>
    requires(std::is_integral_v<T>)
T decodeVarint(BufferStream&);

// allow decoding directly to enum types
template <typename T>
    requires(std::is_enum_v<T>)
T decodeVarint(BufferStream& tileData) {
    return static_cast<T>(decodeVarint<std::make_unsigned_t<std::underlying_type_t<T>>>(tileData));
}

template <>
inline std::uint32_t decodeVarint(BufferStream& buffer) {
    // Max 4 bytes supported
    auto b = buffer.read<std::uint8_t>();
    auto value = static_cast<std::uint32_t>(b & 0x7f);
    if (b & 0x80) {
        b = buffer.read<std::uint8_t>();
        value |= static_cast<std::uint32_t>(b & 0x7f) << 7;
        if (b & 0x80) {
            b = buffer.read<std::uint8_t>();
            value |= static_cast<std::uint32_t>(b & 0x7f) << 14;
            if (b & 0x80) {
                b = buffer.read<std::uint8_t>();
                value |= static_cast<std::uint32_t>(b & 0x7f) << 21;
                if (b & 0x80) {
                    const auto v = static_cast<std::uint32_t>(buffer.read<std::uint8_t>() & 0x7f);
                    if (v > 0x0f) {
                        throw std::runtime_error("varint exceeds 32 bits");
                    }
                    value |= v << 28;
                }
            }
        }
    }
    return value;
}

template <>
inline std::uint64_t decodeVarint(BufferStream& buffer) {
    std::uint64_t value = 0;
    for (int shift = 0; buffer.available();) {
        auto b = buffer.read<std::uint8_t>();
        value |= static_cast<std::uint64_t>(b & 0x7F) << shift;

        if (shift == 63 && b > 1) {
            throw std::runtime_error("Varint too long");
        }
        if ((b & 0x80) == 0) {
            break;
        }

        shift += 7;
        if (shift >= 64) {
            throw std::runtime_error("Varint too long");
        }
    }
    return value;
}

/// Decode N varints, retrurning the values in a `std::tuple`
template <typename T, std::size_t N>
    requires(std::is_integral_v<T> || std::is_enum_v<T>, 0 < N)
auto decodeVarints(BufferStream& buffer) {
    auto v = std::make_tuple(static_cast<T>(decodeVarint<std::uint32_t>(buffer)));
    if constexpr (N == 1) {
        return v;
    } else
        return std::tuple_cat(std::move(v), decodeVarints<T, N - 1>(buffer));
}

/// Decode N varints into the provided buffer
/// Each result is cast to the target type.
template <typename TDecode, typename TTarget = TDecode>
    requires(std::is_integral_v<TDecode> && (std::is_integral_v<TTarget> || std::is_enum_v<TTarget>) &&
             sizeof(TDecode) <= sizeof(TTarget))
void decodeVarints(BufferStream& buffer, const std::uint32_t numValues, TTarget* out) {
    std::generate_n(out, numValues, [&buffer]() { return static_cast<TTarget>(decodeVarint<TDecode>(buffer)); });
}

} // namespace mlt::util::decoding
