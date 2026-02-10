#pragma once

#include <mlt/util/buffer_stream.hpp>
#include <mlt/util/noncopyable.hpp>

#include <algorithm>
#include <functional>
#include <type_traits>

namespace mlt::metadata::stream {
class StreamMetadata;
}

namespace mlt::util::decoding::rle {

namespace detail {
// Borrowed from https://github.com/apache/orc, `/c++/src/ByteRLE.cc`, `ByteRleDecoderImpl::nextInternal`
// Apache License 2.0
class ByteRleDecoder : public util::noncopyable {
public:
    ByteRleDecoder() = delete;
    ByteRleDecoder(const std::uint8_t* buffer, size_t length) noexcept
        : bufferStart(reinterpret_cast<const char*>(buffer)),
          bufferEnd(bufferStart + length) {}
    ByteRleDecoder(ByteRleDecoder&&) = delete;
    ByteRleDecoder& operator=(const ByteRleDecoder&) = delete;
    ByteRleDecoder& operator=(ByteRleDecoder&&) = delete;

    std::size_t getBufferRemaining() const noexcept { return bufferEnd - bufferStart; }

    void next(std::uint8_t* data, std::uint64_t numValues) {
        std::uint64_t position = 0;

        while (position < numValues) {
            // if we are out of values, read more
            if (remainingValues == 0) {
                readHeader();
            }
            // how many do we read out of this block?
            size_t count = std::min(static_cast<size_t>(numValues - position), remainingValues);
            uint64_t consumed = 0;
            if (repeating) {
                std::fill_n(data + position, count, value);
                consumed = count;
            } else {
                uint64_t i = 0;
                while (i < count) {
                    uint64_t copyBytes = std::min(static_cast<uint64_t>(count - i),
                                                  static_cast<uint64_t>(bufferEnd - bufferStart));
                    std::copy(bufferStart, bufferStart + copyBytes, data + position + i);
                    if (!copyBytes) {
                        // prevent infinite loop
                        throw std::runtime_error("Unexpected end of buffer");
                    }
                    bufferStart += copyBytes;
                    i += copyBytes;
                }
                consumed = count;
            }
            remainingValues -= consumed;
            position += count;
        }
    }

private:
    char readByte() {
        if (bufferStart == bufferEnd) {
            throw std::runtime_error("Unexpected end of buffer");
        }
        return *bufferStart++;
    }

    void readHeader() {
        const std::uint8_t ch = readByte();
        if (ch & (1 << 7)) {
            remainingValues = (ch ^ 0xff) + 1;
            repeating = false;
        } else {
            remainingValues = ch + MINIMUM_REPEAT;
            repeating = true;
            value = readByte();
        }
    }

    static constexpr size_t MINIMUM_REPEAT = 3;
    size_t remainingValues = 0;
    char value = 0;
    const char* bufferStart;
    const char* bufferEnd;
    bool repeating = false;
};
} // namespace detail

/// Decode RLE bytes to a byte array
/// @param tileData The source data
/// @param out The target for output
/// @param numBytes The number of bytes to write, and the size of `out`
/// @param byteSize The number of bytes to consume from the source buffer
/// @throws std::runtime_error The provided buffer does not contain enough data
inline void decodeByte(BufferStream& tileData, std::uint8_t* out, std::uint32_t numBytes, std::uint32_t byteSize) {
    detail::ByteRleDecoder{tileData.getData(), tileData.getSize()}.next(out, numBytes);
    tileData.consume(byteSize);
}

/// Decode RLE bits to a vector
/// @param consume Whether to consume input the source buffer (true) or leave it unchanged (false)
void decodeBoolean(BufferStream&, std::vector<uint8_t>&, const metadata::stream::StreamMetadata&, bool consume);

/// Decode RLE bits to int/long values
/// @param in The source data
/// @param out The target for output, must already be sized to contain the resulting data
/// @param numRuns The number of RLE runs in the input
/// @throws std::runtime_error The provided buffer does not contain enough data
template <typename T, typename TTarget = T, typename F = std::function<TTarget(T)>>
    requires(std::is_integral_v<T> && (std::is_integral_v<TTarget> || std::is_enum_v<TTarget>) &&
             sizeof(T) <= sizeof(TTarget) && std::regular_invocable<F, T> &&
             std::is_same_v<std::invoke_result_t<F, T>, TTarget>)
void decodeInt(
    const T* const in,
    const std::size_t inCount,
    TTarget* const out,
    const std::size_t outCount,
    const std::uint32_t numRuns,
    std::function<TTarget(T)> convert = [](T x) { return static_cast<TTarget>(x); }) {
    std::uint32_t inOffset = 0;
    std::uint32_t outOffset = 0;
    for (std::uint32_t i = 0; i < numRuns; ++i) {
        if (inCount < inOffset + 2) {
            throw std::runtime_error("Unexpected end of buffer");
        }
        const T runLength = in[inOffset];
        const TTarget runValue = convert(in[inOffset++ + numRuns]);
        if (outCount < outOffset + runLength) {
            throw std::runtime_error("Unexpected end of buffer");
        }

        std::fill_n(std::next(out, outOffset), runLength, runValue);
        outOffset += runLength;
    }
}

} // namespace mlt::util::decoding::rle
