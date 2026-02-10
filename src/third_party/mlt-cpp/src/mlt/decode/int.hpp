#pragma once

#include <mlt/metadata/stream.hpp>
#include <mlt/util/buffer_stream.hpp>
#include <mlt/util/noncopyable.hpp>
#include <mlt/util/rle.hpp>
#include <mlt/util/vectorized.hpp>
#include <mlt/util/zigzag.hpp>

#include <cstdint>
#include <type_traits>
#include <vector>

namespace mlt::decoder {

class IntegerDecoder : public util::noncopyable {
public:
    using StreamMetadata = metadata::stream::StreamMetadata;
    using MortonEncodedStreamMetadata = metadata::stream::MortonEncodedStreamMetadata;

    IntegerDecoder();
    ~IntegerDecoder() noexcept;

    IntegerDecoder(IntegerDecoder&&) = delete;
    // FastPFOR classes have implicitly-deleted assignment operators.
    // We could create new ones, if really necessary.
    IntegerDecoder& operator=(IntegerDecoder&&) = delete;

    /// Decode a buffer of integers into another, according to the encoding scheme specified by the metadata
    /// @param values Input values
    /// @param out Output values (should be sized according to `getIntArrayBufferSize`)
    /// @param outCount Number of elements in the output buffer
    /// @param metadata Stream metadata specifying the encoding details
    template <typename T, typename TTarget = T>
        requires((std::is_integral_v<T> || std::is_enum_v<T>) &&
                 (std::is_integral_v<TTarget> || std::is_enum_v<TTarget>) && sizeof(T) <= sizeof(TTarget))
    void decodeIntArray(const T* values,
                        std::size_t count,
                        TTarget* out,
                        std::size_t outCount,
                        const StreamMetadata&,
                        bool isSigned = std::is_signed_v<T>);

    /// Decode an integer stream into the target buffer
    /// @param tileData source data
    /// @param out output data, automatically resized
    /// @param metadata stream metadata specifying the encoding details
    /// @details Uses an internal buffer for intermediate values
    template <typename TDecode, typename TInt = TDecode, typename TTarget = TDecode>
    void decodeIntStream(BufferStream& tileData,
                         std::vector<TTarget>& out,
                         const StreamMetadata&,
                         bool isSigned = std::is_signed_v<TDecode>);

    /// Decode an integer stream into the target buffer
    /// @param tileData source data
    /// @param buffer storage for intermediate values, automatically resized
    /// @param out output data, automatically resized
    /// @param metadata stream metadata specifying the encoding details
    template <typename TDecode, typename TInt = TDecode, typename TTarget = TInt>
    void decodeIntStream(BufferStream& tileData,
                         TInt* buffer,
                         std::size_t bufferSize,
                         std::vector<TTarget>& out,
                         const StreamMetadata&,
                         bool isSigned = std::is_signed_v<TDecode>);

    template <typename TDecode, typename TInt = TDecode, typename TTarget = TInt>
    TTarget decodeConstIntStream(BufferStream& tileData,
                                 const StreamMetadata&,
                                 bool isSigned = std::is_signed_v<TDecode>);

    template <typename TDecode, typename TInt = TDecode, typename TTarget = TInt, bool Delta = true>
    void decodeMortonStream(BufferStream& tileData,
                            std::vector<TTarget>& out,
                            const MortonEncodedStreamMetadata& metadata);

    template <typename TDecode, typename TInt = TDecode, typename TTarget = TInt, bool Delta = true>
    void decodeMortonStream(BufferStream& tileData,
                            TInt* buffer,
                            std::size_t bufferSize,
                            TTarget* out,
                            std::size_t outCount,
                            const MortonEncodedStreamMetadata&);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    std::vector<std::uint8_t> buffer;

    /// Get a buffer for intermediate values, which must not be retained beyond the current method invocation
    template <typename T>
    T* getTempBuffer(std::size_t minSize) {
        buffer.resize(std::max(buffer.size(), minSize * sizeof(T)));
        return reinterpret_cast<T*>(buffer.data());
    }

    /// Get the size of the buffer necessary for `decodeIntArray`
    std::size_t getIntArrayBufferSize(const std::size_t count, const StreamMetadata&);

    template <typename TDecode, typename TTarget = TDecode>
        requires(std::is_integral_v<TDecode> && (std::is_integral_v<TTarget> || std::is_enum_v<TTarget>))
    void decodeStream(BufferStream& tileData, TTarget* out, std::size_t outSize, const StreamMetadata&);

    template <typename T>
    static void decodeRLE(const std::vector<T>& values, std::vector<T>& out, const std::uint32_t numRuns);

    template <typename T>
    static void decodeDeltaRLE(const std::vector<T>& values, std::vector<T>& out, const std::uint32_t numRuns);

    /// Decode zigzag-delta values
    /// @param values Input values
    /// @param count Count of input values
    /// @param out Output values, must be the same size as the input
    /// @param outCount Count of available output values
    /// @note Input and output may reference the same memory
    template <typename T, typename TTarget>
        requires(std::is_integral_v<underlying_type_t<T>> && std::is_integral_v<underlying_type_t<TTarget>> &&
                 sizeof(T) <= sizeof(TTarget))
    static void decodeZigZagDelta(const T* values,
                                  const std::size_t count,
                                  TTarget* const out,
                                  const std::size_t outCount) noexcept;

    /// Decode standard or delta Morton codes
    /// @param data Input data
    /// @param count Number of input values
    /// @param out Output data, must be 2x the input size
    /// @param outCount Number of available output values
    template <typename TDecode, typename TTarget, bool delta>
        requires(std::is_integral_v<TDecode> && (std::is_integral_v<TTarget> || std::is_enum_v<TTarget>))
    static void decodeMortonCodes(const TDecode* const data,
                                  const std::size_t count,
                                  TTarget* out,
                                  std::size_t outCount,
                                  int numBits,
                                  int coordinateShift) noexcept;

    std::uint32_t decodeFastPfor(BufferStream& buffer,
                                 std::uint32_t* const result,
                                 const std::size_t numValues,
                                 const std::size_t byteLength);
};
} // namespace mlt::decoder
