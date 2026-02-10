#pragma once

#include <mlt/common.hpp>
#include <mlt/decode/int.hpp>
#include <mlt/util/morton_curve.hpp>

#include <cassert>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace mlt::decoder {

#pragma region Public

inline std::size_t IntegerDecoder::getIntArrayBufferSize(const std::size_t count,
                                                         const StreamMetadata& streamMetadata) {
    using namespace metadata::stream;
    switch (streamMetadata.getLogicalLevelTechnique1()) {
        case LogicalLevelTechnique::DELTA:
            if (streamMetadata.getLogicalLevelTechnique2() == LogicalLevelTechnique::RLE &&
                streamMetadata.getMetadataType() == LogicalLevelTechnique::RLE) {
                const auto& rleMetadata = static_cast<const RleEncodedStreamMetadata&>(streamMetadata);
                return rleMetadata.getNumRleValues();
            }
            return streamMetadata.getNumValues();
        case LogicalLevelTechnique::NONE:
        case LogicalLevelTechnique::COMPONENTWISE_DELTA:
            return count;
        case LogicalLevelTechnique::RLE: {
            if (streamMetadata.getMetadataType() != LogicalLevelTechnique::RLE) {
                return 0;
            }
            const auto& rleMetadata = static_cast<const RleEncodedStreamMetadata&>(streamMetadata);
            return rleMetadata.getNumRleValues();
        }
        case LogicalLevelTechnique::MORTON: {
            return 2 * count;
        }
        case LogicalLevelTechnique::PSEUDODECIMAL:
        default:
            return 0;
    }
}

template <typename T, typename TTarget>
    requires((std::is_integral_v<T> || std::is_enum_v<T>) && (std::is_integral_v<TTarget> || std::is_enum_v<TTarget>) &&
             sizeof(T) <= sizeof(TTarget))
void IntegerDecoder::decodeIntArray(const T* values,
                                    const std::size_t count,
                                    TTarget* out,
                                    const std::size_t outCount,
                                    const StreamMetadata& streamMetadata,
                                    const bool isSigned) {
    using namespace metadata::stream;
    using namespace util::decoding;
    switch (streamMetadata.getLogicalLevelTechnique1()) {
        case LogicalLevelTechnique::NONE: {
            assert(count <= outCount);
            const auto f = isSigned ? [](T x) noexcept { return static_cast<TTarget>(decodeZigZag(x)); } : [](T x) noexcept { return static_cast<TTarget>(x); };
            std::transform(values, values + count, out, f);
            return;
        }
        case LogicalLevelTechnique::DELTA:
            if (streamMetadata.getLogicalLevelTechnique2() == LogicalLevelTechnique::RLE) {
                if (streamMetadata.getMetadataType() != LogicalLevelTechnique::RLE) {
                    throw std::runtime_error("invalid RLE metadata");
                }
                const auto& rleMetadata = static_cast<const RleEncodedStreamMetadata&>(streamMetadata);
                assert(outCount >= rleMetadata.getNumRleValues());
                rle::decodeInt<T, TTarget>(values, count, out, outCount, rleMetadata.getRuns());
                decodeZigZagDelta(out, outCount, out, outCount);
            } else {
                assert(outCount >= count);
                decodeZigZagDelta(values, count, out, outCount);
            }
            break;
        case LogicalLevelTechnique::COMPONENTWISE_DELTA:
            if constexpr (std::is_same_v<TTarget, std::int32_t> || std::is_same_v<TTarget, std::uint32_t>) {
                assert(count <= outCount);
                std::transform(values, values + count, out, [](auto x) { return static_cast<TTarget>(x); });
                vectorized::decodeComponentwiseDeltaVec2(out, outCount);
                break;
            }
            throw std::runtime_error("Logical level technique COMPONENTWISE_DELTA not implemented for 64-bit values");
        case LogicalLevelTechnique::RLE: {
            if (streamMetadata.getMetadataType() != LogicalLevelTechnique::RLE) {
                throw std::runtime_error("invalid RLE metadata");
            }
            const auto& rleMetadata = static_cast<const RleEncodedStreamMetadata&>(streamMetadata);
            assert(rleMetadata.getNumRleValues() <= outCount);
            auto decoder = isSigned ? [](T x) {
                    return static_cast<TTarget>(decodeZigZag(x));
            } : [](T x) {
                    return static_cast<TTarget>(x);
            };
            rle::decodeInt<T, TTarget>(values, count, out, outCount, rleMetadata.getRuns(), std::move(decoder));
            break;
        }
        case LogicalLevelTechnique::MORTON: {
            // TODO: zig-zag decode when morton second logical level technique
            if (streamMetadata.getMetadataType() != LogicalLevelTechnique::MORTON) {
                throw std::runtime_error("invalid RLE metadata");
            }
            const auto& mortonMetadata = static_cast<const MortonEncodedStreamMetadata&>(streamMetadata);
            assert(2 * count <= outCount);
            if constexpr (std::is_same_v<T, std::uint32_t> && std::is_same_v<TTarget, std::uint32_t>) {
                decodeMortonCodes<T, TTarget, true>(
                    values, count, out, outCount, mortonMetadata.getNumBits(), mortonMetadata.getCoordinateShift());
            } else {
                throw std::runtime_error("Logical level technique MORTON not implemented for 64-bit values");
            }
            break;
        }
        case LogicalLevelTechnique::PSEUDODECIMAL:
        default:
            throw std::runtime_error("The specified logical level technique is not supported for integers: " +
                                     std::to_string(std::to_underlying(streamMetadata.getLogicalLevelTechnique1())));
            break;
    }
}

template <typename TDecode, typename TInt, typename TTarget>
void IntegerDecoder::decodeIntStream(BufferStream& tileData,
                                     std::vector<TTarget>& out,
                                     const StreamMetadata& metadata,
                                     const bool isSigned) {
    auto* tempBuffer = getTempBuffer<TInt>(metadata.getNumValues());
    decodeIntStream<TDecode, TInt, TTarget>(tileData, tempBuffer, metadata.getNumValues(), out, metadata, isSigned);
}

template <typename TDecode, typename TInt, typename TTarget>
void IntegerDecoder::decodeIntStream(BufferStream& tileData,
                                     TInt* buffer,
                                     std::size_t bufferSize,
                                     std::vector<TTarget>& out,
                                     const StreamMetadata& metadata,
                                     const bool isSigned) {
    decodeStream<TDecode, TInt>(tileData, buffer, bufferSize, metadata);
    out.resize(getIntArrayBufferSize(bufferSize, metadata));
    decodeIntArray<TInt, TTarget>(buffer, bufferSize, out.data(), out.size(), metadata, isSigned);
}

template <typename TDecode, typename TInt, typename TTarget>
TTarget IntegerDecoder::decodeConstIntStream(BufferStream& tileData,
                                             const StreamMetadata& metadata,
                                             const bool isSigned) {
    TInt buffer[2] = {0};
    decodeStream<TDecode, TInt>(tileData, &buffer[0], countof(buffer), metadata);

    if (metadata.getNumValues() < 1 || metadata.getNumValues() > 2) {
        throw std::runtime_error("unexpected number of values in constant stream");
    }

    if (metadata.getNumValues() == 1) {
        return isSigned ? decodeZigZagValue(buffer[0]) : buffer[0];
    }
    return isSigned ? decodeZigZagValue(buffer[1]) : buffer[1];
}

template <typename TDecode, typename TInt, typename TTarget, bool Delta>
void IntegerDecoder::decodeMortonStream(BufferStream& tileData,
                                        std::vector<TTarget>& out,
                                        const MortonEncodedStreamMetadata& metadata) {
    auto* tempBuffer = getTempBuffer<TInt>(metadata.getNumValues());
    out.resize(2 * metadata.getNumValues());
    decodeMortonStream<TDecode, TInt, TTarget, Delta>(
        tileData, tempBuffer, metadata.getNumValues(), out.data(), out.size(), metadata);
}

template <typename TDecode, typename TInt, typename TTarget, bool Delta>
void IntegerDecoder::decodeMortonStream(BufferStream& tileData,
                                        TInt* buffer,
                                        std::size_t bufferCount,
                                        TTarget* out,
                                        std::size_t outCount,
                                        const MortonEncodedStreamMetadata& metadata) {
    decodeStream<TDecode, TInt>(tileData, buffer, bufferCount, metadata);
    assert(outCount == 2 * bufferCount);
    decodeMortonCodes<TInt, TTarget, Delta>(
        buffer, bufferCount, out, outCount, metadata.getNumBits(), metadata.getCoordinateShift());
}

#pragma endregion

#pragma region Private

template <typename TDecode, typename TTarget>
    requires(std::is_integral_v<TDecode> && (std::is_integral_v<TTarget> || std::is_enum_v<TTarget>))
void IntegerDecoder::decodeStream(BufferStream& tileData,
                                  TTarget* out,
                                  std::size_t outSize,
                                  const StreamMetadata& metadata) {
    using namespace metadata::stream;

    assert(outSize >= metadata.getNumValues());

    switch (metadata.getPhysicalLevelTechnique()) {
        case PhysicalLevelTechnique::FAST_PFOR: {
            std::uint32_t* outPtr = nullptr;
            if constexpr (sizeof(*out) == sizeof(std::uint32_t)) {
                // Decode directly into the output buffer
                outPtr = reinterpret_cast<std::uint32_t*>(out);
            } else {
                // Decode into a 32-bit temprary buffer
                outPtr = getTempBuffer<std::uint32_t>(metadata.getNumValues());
            }

            const auto resultLength = decodeFastPfor(
                tileData, outPtr, metadata.getNumValues(), metadata.getByteLength());
            if (resultLength != outSize) {
                throw std::runtime_error("Unexpected decode result (" + std::to_string(resultLength) + "," +
                                         std::to_string(outSize) + ")");
            }

            if constexpr (sizeof(*out) != sizeof(std::uint32_t)) {
                // ... then extend to 64-bit output (`.mapToLong(i -> i)` in Java)
                std::transform(outPtr, outPtr + resultLength, out, [](auto x) { return static_cast<TTarget>(x); });
            }

            break;
        }
        case PhysicalLevelTechnique::VARINT:
            util::decoding::decodeVarints<TDecode>(tileData, static_cast<std::uint32_t>(outSize), out);
            break;
        default:
            throw std::runtime_error("Specified physical level technique not yet supported " +
                                     std::to_string(std::to_underlying(metadata.getPhysicalLevelTechnique())));
    }
}

template <typename T>
void IntegerDecoder::decodeRLE(const std::vector<T>& values, std::vector<T>& out, const std::uint32_t numRuns) {
    std::uint32_t outPos = 0;
    for (std::uint32_t i = 0; i < numRuns; ++i) {
        const auto run = values[i];
        const auto value = values[i + numRuns];
        if (outPos + run > out.size()) {
            throw std::runtime_error("RLE run exceeds output buffer size");
        }
        std::fill(std::next(out.begin(), outPos), std::next(out.begin(), outPos + run), value);
    }
}

template <typename T>
void IntegerDecoder::decodeDeltaRLE(const std::vector<T>& values, std::vector<T>& out, const std::uint32_t numRuns) {
    std::uint32_t outPos = 0;
    T previousValue = 0;
    for (std::uint32_t i = 0; i < numRuns; ++i) {
        const auto run = values[i];
        if (outPos + run > out.size()) {
            throw std::runtime_error("RLE run exceeds output buffer size");
        }

        const auto value = static_cast<std::make_signed_t<T>>(values[i + numRuns]);
        const auto delta = util::decoding::decodeZigZag(value);
        for (std::size_t j = 0; j < run; ++j) {
            out[outPos++] = static_cast<T>(previousValue += delta);
        }
    }
}

template <typename T, typename TTarget>
    requires(std::is_integral_v<underlying_type_t<T>> && std::is_integral_v<underlying_type_t<TTarget>> &&
             sizeof(T) <= sizeof(TTarget))
void IntegerDecoder::decodeZigZagDelta(const T* values,
                                       const std::size_t count,
                                       TTarget* const out,
                                       const std::size_t outCount) noexcept {
    using namespace util::decoding;
    assert(count == outCount);
    std::uint32_t pos = 0;
    using ST = std::make_signed_t<underlying_type_t<T>>;
    ST previousValue = 0;
    for (const auto zigZagDelta : std::span{values, count}) {
        const auto delta = static_cast<ST>(decodeZigZag(zigZagDelta));
        out[pos++] = static_cast<TTarget>(previousValue += delta);
    }
}

template <typename TDecode, typename TTarget, bool delta>
    requires(std::is_integral_v<TDecode> && (std::is_integral_v<TTarget> || std::is_enum_v<TTarget>))
void IntegerDecoder::decodeMortonCodes(const TDecode* const data,
                                       const std::size_t count,
                                       TTarget* const out,
                                       std::size_t outCount,
                                       int numBits,
                                       int coordinateShift) noexcept {
    using namespace util::decoding;
    assert(outCount == 2 * count);
    std::uint32_t previousMortonCode = 0;
    for (std::size_t i = 0, j = 0; i < count; ++i) {
        auto mortonCode = static_cast<std::uint32_t>(data[i]);
        if constexpr (delta) {
            mortonCode += previousMortonCode;
        };
        out[j++] = static_cast<TTarget>(util::MortonCurve::decode(mortonCode, numBits) - coordinateShift);
        out[j++] = static_cast<TTarget>(util::MortonCurve::decode(mortonCode >> 1, numBits) - coordinateShift);
        if constexpr (delta) {
            previousMortonCode = mortonCode;
        }
    }
}

#pragma endregion Private

} // namespace mlt::decoder
