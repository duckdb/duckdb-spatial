#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace mlt::util::encoding::rle {

// ORC-style byte RLE: control byte high bit set → literal run, otherwise repeated run.
inline void encodeByte(const std::uint8_t* data, std::size_t count, std::vector<std::uint8_t>& out) {
    static constexpr std::size_t MIN_REPEAT = 3;
    static constexpr std::size_t MAX_REPEAT = 127 + MIN_REPEAT;
    static constexpr std::size_t MAX_LITERAL = 128;

    std::size_t pos = 0;
    while (pos < count) {
        std::size_t runLength = 1;
        while (pos + runLength < count && data[pos + runLength] == data[pos] && runLength < MAX_REPEAT) {
            ++runLength;
        }

        if (runLength >= MIN_REPEAT) {
            out.push_back(static_cast<std::uint8_t>(runLength - MIN_REPEAT));
            out.push_back(data[pos]);
            pos += runLength;
        } else {
            const auto literalStart = pos;
            std::size_t literalCount = 0;
            while (pos < count && literalCount < MAX_LITERAL) {
                std::size_t ahead = 1;
                while (pos + ahead < count && data[pos + ahead] == data[pos] && ahead < MIN_REPEAT) {
                    ++ahead;
                }
                if (ahead >= MIN_REPEAT) {
                    break;
                }
                ++pos;
                ++literalCount;
            }
            out.push_back(static_cast<std::uint8_t>(0xFF - (literalCount - 1)));
            out.insert(out.end(), data + literalStart, data + literalStart + literalCount);
        }
    }
}

inline std::vector<std::uint8_t> encodeBooleanRle(const std::uint8_t* bits, std::uint32_t numBits) {
    const auto numBytes = (numBits + 7) / 8;
    std::vector<std::uint8_t> result;
    result.reserve(numBytes);
    encodeByte(bits, numBytes, result);
    return result;
}

template <typename T>
    requires(std::is_integral_v<T>)
struct IntRleResult {
    std::vector<T> runs;
    std::vector<T> values;
};

template <typename T>
    requires(std::is_integral_v<T>)
IntRleResult<T> encodeIntRle(std::span<const T> data) {
    IntRleResult<T> result;
    if (data.empty()) {
        return result;
    }

    T currentValue = data[0];
    T currentRun = 1;

    for (std::size_t i = 1; i < data.size(); ++i) {
        if (data[i] == currentValue) {
            ++currentRun;
        } else {
            result.runs.push_back(currentRun);
            result.values.push_back(currentValue);
            currentValue = data[i];
            currentRun = 1;
        }
    }
    result.runs.push_back(currentRun);
    result.values.push_back(currentValue);
    return result;
}

} // namespace mlt::util::encoding::rle
