#include <algorithm>
#include <mlt/encode/int.hpp>
#include <mlt/encoder.hpp>
#include <mlt/metadata/stream.hpp>
#include <mlt/util/encoding/buffer.hpp>
#include <mlt/util/encoding/zigzag.hpp>

#if MLT_WITH_FASTPFOR
#include <codecs.h>
#include <compositecodec.h>
#include <fastpfor.h>
#include <stdexcept>
#include <variablebyte.h>
#endif

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace mlt::encoder {

using metadata::stream::LogicalLevelTechnique;
using metadata::stream::LogicalStreamType;
using metadata::stream::PhysicalLevelTechnique;
using metadata::stream::PhysicalStreamType;
using metadata::stream::RleEncodedStreamMetadata;
using metadata::stream::StreamMetadata;

namespace {

struct Candidate {
    LogicalLevelTechnique t1;
    LogicalLevelTechnique t2;
    std::vector<std::uint8_t>* data;
    std::uint32_t numRuns;
    std::uint32_t physicalLength;
};

template <typename TInt>
std::vector<std::uint8_t> encodeVarintsIntegral(std::span<const TInt> values, bool zigZagForSigned = false) {
    static_assert(std::is_integral_v<TInt>, "encodeVarintsIntegral requires integral type");

    std::vector<std::uint8_t> result;
    result.reserve(values.size() * (sizeof(TInt) <= sizeof(std::uint32_t) ? 2 : 3));
    for (const auto v : values) {
        if constexpr (std::is_signed_v<TInt>) {
            const auto encoded = zigZagForSigned ? util::encoding::encodeZigZag(v)
                                                 : static_cast<std::make_unsigned_t<TInt>>(v);
            util::encoding::encodeVarint(encoded, result);
        } else {
            util::encoding::encodeVarint(v, result);
        }
    }
    return result;
}

#if MLT_WITH_FASTPFOR
template <typename TInput, typename Transform, typename TCodec>
std::vector<std::uint8_t> encodeFastPforCore(std::span<const TInput> values, Transform transform, TCodec& codec) {
    std::vector<std::uint32_t> input(values.size());
    std::ranges::transform(values, input.begin(), transform);

    // FastPFor can throw NotEnoughStorage for incompressible/alignment-sensitive inputs.
    // Start with the common-case size and retry with larger buffers as needed.
    std::vector<std::uint32_t> compressed(input.size() + 1024);
    std::size_t compressedSize = 0;
    bool encoded = false;
    for (int attempt = 0; attempt < 4 && !encoded; ++attempt) {
        compressedSize = compressed.size();
        try {
            codec.encodeArray(input.data(), input.size(), compressed.data(), compressedSize);
            encoded = true;
        } catch (const FastPForLib::NotEnoughStorage&) {
            compressed.resize(compressed.size() * 2);
        }
    }

    if (!encoded) {
        throw std::runtime_error("FastPFOR encoding failed: insufficient output buffer");
    }

    std::vector<std::uint8_t> result(compressedSize * sizeof(std::uint32_t));
    auto* outWords = reinterpret_cast<std::uint32_t*>(result.data());
    for (std::size_t i = 0; i < compressedSize; ++i) {
        outWords[i] = std::byteswap(compressed[i]);
    }
    return result;
}
#endif

template <typename TInt, typename EncodeFn>
IntegerEncodingResult encodeSignedIntegral(std::span<const TInt> values,
                                           bool isSigned,
                                           IntegerEncodingOption option,
                                           EncodeFn encode,
                                           bool preferConstRleWhenSingleRun) {
    auto plainEncoded = encode(values, isSigned);

    std::vector<TInt> deltaValues(values.size());
    TInt previousValue = 0;
    TInt previousDelta = 0;
    std::uint32_t runs = 1;
    std::uint32_t deltaRuns = 1;
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto value = values[i];
        const auto delta = static_cast<TInt>(value - previousValue);
        deltaValues[i] = delta;

        if (i != 0 && value != previousValue) {
            ++runs;
        }
        if (i != 0 && delta != previousDelta) {
            ++deltaRuns;
        }
        previousValue = value;
        previousDelta = delta;
    }

    auto deltaEncoded = encode(deltaValues, /*zigZag=*/true);

    Candidate best{.t1 = LogicalLevelTechnique::NONE,
                   .t2 = LogicalLevelTechnique::NONE,
                   .data = &plainEncoded,
                   .numRuns = 0,
                   .physicalLength = static_cast<std::uint32_t>(values.size())};

    if (deltaEncoded.size() < best.data->size()) {
        best = {.t1 = LogicalLevelTechnique::DELTA,
                .t2 = LogicalLevelTechnique::NONE,
                .data = &deltaEncoded,
                .numRuns = 0,
                .physicalLength = static_cast<std::uint32_t>(values.size())};
    }

    std::vector<std::uint8_t> rleEncoded;
    std::uint32_t rlePhysicalLength = 0;
    bool isConstStream = false;
    const bool shouldTryRle = option != IntegerEncodingOption::AUTO || values.size() / runs >= 2;
    if (shouldTryRle) {
        auto rle = util::encoding::rle::encodeIntRle<TInt>(values);
        isConstStream = (rle.runs.size() == 1);

        std::vector<TInt> flattened;
        flattened.reserve(rle.runs.size() + rle.values.size());
        flattened.insert(flattened.end(), rle.runs.begin(), rle.runs.end());
        if (isSigned) {
            for (auto v : rle.values) {
                flattened.push_back(static_cast<TInt>(util::encoding::encodeZigZag(v)));
            }
        } else {
            flattened.insert(flattened.end(), rle.values.begin(), rle.values.end());
        }
        rleEncoded = encode(flattened, /*zigZag=*/false);

        rlePhysicalLength = static_cast<std::uint32_t>(rle.runs.size() + rle.values.size());
        if ((preferConstRleWhenSingleRun && isConstStream) || rleEncoded.size() < best.data->size()) {
            best = {.t1 = LogicalLevelTechnique::RLE,
                    .t2 = LogicalLevelTechnique::NONE,
                    .data = &rleEncoded,
                    .numRuns = runs,
                    .physicalLength = rlePhysicalLength};
        }
    }

    std::vector<std::uint8_t> deltaRleEncoded;
    std::uint32_t deltaRlePhysicalLength = 0;
    const bool shouldTryDeltaRle = option != IntegerEncodingOption::AUTO ||
                                   (deltaValues.size() / deltaRuns >= 2 &&
                                    (!preferConstRleWhenSingleRun || !isConstStream));
    if (shouldTryDeltaRle) {
        const auto deltaRle = util::encoding::rle::encodeIntRle<TInt>(deltaValues);

        std::vector<TInt> flattened;
        flattened.reserve(deltaRle.runs.size() + deltaRle.values.size());
        flattened.insert(flattened.end(), deltaRle.runs.begin(), deltaRle.runs.end());
        for (auto v : deltaRle.values) {
            flattened.push_back(static_cast<TInt>(util::encoding::encodeZigZag(v)));
        }
        deltaRleEncoded = encode(flattened, /*zigZag=*/false);

        deltaRlePhysicalLength = static_cast<std::uint32_t>(deltaRle.runs.size() + deltaRle.values.size());
        if (deltaRleEncoded.size() < best.data->size()) {
            best = {.t1 = LogicalLevelTechnique::DELTA,
                    .t2 = LogicalLevelTechnique::RLE,
                    .data = &deltaRleEncoded,
                    .numRuns = deltaRuns,
                    .physicalLength = deltaRlePhysicalLength};
        }
    }

    switch (option) {
        case IntegerEncodingOption::AUTO:
            break;
        case IntegerEncodingOption::PLAIN:
            best = {.t1 = LogicalLevelTechnique::NONE,
                    .t2 = LogicalLevelTechnique::NONE,
                    .data = &plainEncoded,
                    .numRuns = 0,
                    .physicalLength = static_cast<std::uint32_t>(values.size())};
            break;
        case IntegerEncodingOption::DELTA:
            best = {.t1 = LogicalLevelTechnique::DELTA,
                    .t2 = LogicalLevelTechnique::NONE,
                    .data = &deltaEncoded,
                    .numRuns = 0,
                    .physicalLength = static_cast<std::uint32_t>(values.size())};
            break;
        case IntegerEncodingOption::RLE:
            best = {.t1 = LogicalLevelTechnique::RLE,
                    .t2 = LogicalLevelTechnique::NONE,
                    .data = &rleEncoded,
                    .numRuns = runs,
                    .physicalLength = rlePhysicalLength};
            break;
        case IntegerEncodingOption::DELTA_RLE:
            best = {.t1 = LogicalLevelTechnique::DELTA,
                    .t2 = LogicalLevelTechnique::RLE,
                    .data = &deltaRleEncoded,
                    .numRuns = deltaRuns,
                    .physicalLength = deltaRlePhysicalLength};
            break;
    }

    return {.logicalLevelTechnique1 = best.t1,
            .logicalLevelTechnique2 = best.t2,
            .encodedValues = std::move(*best.data),
            .numRuns = best.numRuns,
            .physicalLevelEncodedValuesLength = best.physicalLength};
}

template <typename TUInt, typename EncodeFn>
IntegerEncodingResult encodeUnsignedIntegralNoDelta(std::span<const TUInt> values,
                                                    IntegerEncodingOption option,
                                                    EncodeFn encode) {
    auto plainEncoded = encode(values);

    Candidate best{.t1 = LogicalLevelTechnique::NONE,
                   .t2 = LogicalLevelTechnique::NONE,
                   .data = &plainEncoded,
                   .numRuns = 0,
                   .physicalLength = static_cast<std::uint32_t>(values.size())};

    std::uint32_t runs = 1;
    TUInt previousValue = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto value = values[i];
        if (i != 0 && value != previousValue) {
            ++runs;
        }
        previousValue = value;
    }

    std::vector<std::uint8_t> rleEncoded;
    std::uint32_t rlePhysicalLength = 0;
    const bool shouldTryRle = option != IntegerEncodingOption::AUTO || values.size() / runs >= 2;
    if (shouldTryRle) {
        const auto rle = util::encoding::rle::encodeIntRle<TUInt>(values);

        std::vector<TUInt> flattened;
        flattened.reserve(rle.runs.size() + rle.values.size());
        flattened.insert(flattened.end(), rle.runs.begin(), rle.runs.end());
        flattened.insert(flattened.end(), rle.values.begin(), rle.values.end());
        rleEncoded = encode(flattened);

        rlePhysicalLength = static_cast<std::uint32_t>(rle.runs.size() + rle.values.size());
        if (rleEncoded.size() < best.data->size()) {
            best = {.t1 = LogicalLevelTechnique::RLE,
                    .t2 = LogicalLevelTechnique::NONE,
                    .data = &rleEncoded,
                    .numRuns = runs,
                    .physicalLength = rlePhysicalLength};
        }
    }

    switch (option) {
        case IntegerEncodingOption::AUTO:
            break;
        case IntegerEncodingOption::PLAIN:
        case IntegerEncodingOption::DELTA:
            best = {.t1 = LogicalLevelTechnique::NONE,
                    .t2 = LogicalLevelTechnique::NONE,
                    .data = &plainEncoded,
                    .numRuns = 0,
                    .physicalLength = static_cast<std::uint32_t>(values.size())};
            break;
        case IntegerEncodingOption::RLE:
        case IntegerEncodingOption::DELTA_RLE:
            best = {.t1 = LogicalLevelTechnique::RLE,
                    .t2 = LogicalLevelTechnique::NONE,
                    .data = &rleEncoded,
                    .numRuns = runs,
                    .physicalLength = rlePhysicalLength};
            break;
    }

    return {.logicalLevelTechnique1 = best.t1,
            .logicalLevelTechnique2 = best.t2,
            .encodedValues = std::move(*best.data),
            .numRuns = best.numRuns,
            .physicalLevelEncodedValuesLength = best.physicalLength};
}

template <typename TUnsigned, typename TSigned, typename SignedEncodeFn>
std::optional<IntegerEncodingResult> tryEncodeViaSignedPath(std::span<const TUnsigned> values,
                                                            SignedEncodeFn&& encodeSigned) {
    static_assert(std::is_unsigned_v<TUnsigned>, "TUnsigned must be unsigned");
    static_assert(std::is_signed_v<TSigned>, "TSigned must be signed");

    const bool canUseSignedPath = std::ranges::all_of(
        values, [](TUnsigned v) { return v <= static_cast<TUnsigned>(std::numeric_limits<TSigned>::max()); });
    if (!canUseSignedPath) {
        return std::nullopt;
    }

    std::vector<TSigned> signedValues(values.size());
    std::ranges::transform(values, signedValues.begin(), [](TUnsigned v) { return static_cast<TSigned>(v); });
    return encodeSigned(signedValues);
}

template <typename T, typename VarintFn, typename FastPforFn>
auto makeSignedPhysicalEncoder(PhysicalLevelTechnique physicalTechnique,
                               VarintFn&& varintEncode,
                               FastPforFn&& fastPforEncode) {
    return [physicalTechnique,
            varintEncode = std::forward<VarintFn>(varintEncode),
            fastPforEncode = std::forward<FastPforFn>(fastPforEncode)](std::span<const T> input, bool zigZag) {
#if MLT_WITH_FASTPFOR
        if (physicalTechnique == PhysicalLevelTechnique::FAST_PFOR) {
            return fastPforEncode(input, zigZag);
        }
#endif
        return varintEncode(input, zigZag);
    };
}

template <typename T, typename VarintFn, typename FastPforFn>
auto makeUnsignedPhysicalEncoder(PhysicalLevelTechnique physicalTechnique,
                                 VarintFn&& varintEncode,
                                 FastPforFn&& fastPforEncode) {
    return [physicalTechnique,
            varintEncode = std::forward<VarintFn>(varintEncode),
            fastPforEncode = std::forward<FastPforFn>(fastPforEncode)](std::span<const T> input) {
#if MLT_WITH_FASTPFOR
        if (physicalTechnique == PhysicalLevelTechnique::FAST_PFOR) {
            return fastPforEncode(input);
        }
#endif
        return varintEncode(input);
    };
}

template <typename TValue, typename EncodeFn, typename BuildFn>
util::EncodedChunks encodeStreamWithMetadata(std::span<const TValue> values,
                                             PhysicalLevelTechnique physicalTechnique,
                                             PhysicalStreamType streamType,
                                             std::optional<LogicalStreamType> logicalType,
                                             EncodeFn&& encodeValues,
                                             BuildFn&& buildStreamFn) {
    auto encoded = encodeValues(values);
    return buildStreamFn(std::move(encoded),
                         static_cast<std::uint32_t>(values.size()),
                         physicalTechnique,
                         streamType,
                         std::move(logicalType));
}

} // namespace

struct IntegerEncoder::Impl {
#if MLT_WITH_FASTPFOR
    FastPForLib::CompositeCodec<FastPForLib::FastPFor<8>, FastPForLib::VariableByte> codec;
#endif
    IntegerEncodingOption encodingOption = IntegerEncodingOption::AUTO;
};

IntegerEncoder::IntegerEncoder()
    : impl(std::make_unique<Impl>()) {}

IntegerEncoder::~IntegerEncoder() noexcept = default;

void IntegerEncoder::setDefaultEncodingOption(IntegerEncodingOption option) {
    impl->encodingOption = option;
}

std::vector<std::uint8_t> IntegerEncoder::encodeVarints32(std::span<const std::int32_t> values, bool zigZag) {
    return encodeVarintsIntegral(values, zigZag);
}

std::vector<std::uint8_t> IntegerEncoder::encodeVarints64(std::span<const std::int64_t> values, bool zigZag) {
    return encodeVarintsIntegral(values, zigZag);
}

std::vector<std::uint8_t> IntegerEncoder::encodeVarintsUnsigned32(std::span<const std::uint32_t> values) {
    return encodeVarintsIntegral(values);
}

std::vector<std::uint8_t> IntegerEncoder::encodeVarintsUnsigned64(std::span<const std::uint64_t> values) {
    return encodeVarintsIntegral(values);
}

std::vector<std::uint8_t> IntegerEncoder::encodeFastPfor([[maybe_unused]] std::span<const std::int32_t> values,
                                                         [[maybe_unused]] bool zigZag) {
#if MLT_WITH_FASTPFOR
    const auto transform = [zigZag](std::int32_t v) {
        return zigZag ? util::encoding::encodeZigZag(v) : static_cast<std::uint32_t>(v);
    };
    return encodeFastPforCore(values, transform, impl->codec);
#else
    throw std::runtime_error("FastPFOR encoding is not enabled. Configure with MLT_WITH_FASTPFOR=ON");
#endif
}

std::vector<std::uint8_t> IntegerEncoder::encodeFastPforUnsigned(
    [[maybe_unused]] std::span<const std::uint32_t> values) {
#if MLT_WITH_FASTPFOR
    const auto transform = [](std::uint32_t v) {
        return v;
    };
    return encodeFastPforCore(values, transform, impl->codec);
#else
    throw std::runtime_error("FastPFOR encoding is not enabled. Configure with MLT_WITH_FASTPFOR=ON");
#endif
}

IntegerEncodingResult IntegerEncoder::encodeInt(std::span<const std::int32_t> values,
                                                [[maybe_unused]] PhysicalLevelTechnique physicalTechnique,
                                                bool isSigned) {
    return encodeInt(values, physicalTechnique, isSigned, impl->encodingOption);
}

IntegerEncodingResult IntegerEncoder::encodeInt(std::span<const std::int32_t> values,
                                                [[maybe_unused]] PhysicalLevelTechnique physicalTechnique,
                                                bool isSigned,
                                                IntegerEncodingOption option) {
    const auto encode = makeSignedPhysicalEncoder<std::int32_t>(
        physicalTechnique,
        [](std::span<const std::int32_t> input, bool zigZag) { return encodeVarints32(input, zigZag); },
        [this](std::span<const std::int32_t> input, bool zigZag) { return encodeFastPfor(input, zigZag); });
    return encodeSignedIntegral<std::int32_t>(values,
                                              isSigned,
                                              option,
                                              encode,
                                              /*preferConstRleWhenSingleRun=*/true);
}

IntegerEncodingResult IntegerEncoder::encodeLong(std::span<const std::int64_t> values, bool isSigned) {
    return encodeLong(values, isSigned, impl->encodingOption);
}

IntegerEncodingResult IntegerEncoder::encodeLong(std::span<const std::int64_t> values,
                                                 bool isSigned,
                                                 IntegerEncodingOption option) {
    const auto encode = [](std::span<const std::int64_t> input, bool zigZag) {
        return encodeVarints64(input, zigZag);
    };
    return encodeSignedIntegral<std::int64_t>(values,
                                              isSigned,
                                              option,
                                              encode,
                                              /*preferConstRleWhenSingleRun=*/false);
}

IntegerEncodingResult IntegerEncoder::encodeUint32(std::span<const std::uint32_t> values,
                                                   [[maybe_unused]] PhysicalLevelTechnique physicalTechnique) {
    return encodeUint32(values, physicalTechnique, impl->encodingOption);
}

IntegerEncodingResult IntegerEncoder::encodeUint32(std::span<const std::uint32_t> values,
                                                   [[maybe_unused]] PhysicalLevelTechnique physicalTechnique,
                                                   IntegerEncodingOption option) {
    const auto signedEncoded = tryEncodeViaSignedPath<std::uint32_t, std::int32_t>(values, [&](auto& signedValues) {
        return encodeInt(signedValues, physicalTechnique, /*isSigned=*/false, option);
    });
    if (signedEncoded.has_value()) {
        return *signedEncoded;
    }

    const auto encode = makeUnsignedPhysicalEncoder<std::uint32_t>(
        physicalTechnique,
        [](std::span<const std::uint32_t> input) { return encodeVarintsUnsigned32(input); },
        [this](std::span<const std::uint32_t> input) { return encodeFastPforUnsigned(input); });
    return encodeUnsignedIntegralNoDelta<std::uint32_t>(values, option, encode);
}

IntegerEncodingResult IntegerEncoder::encodeUint64(std::span<const std::uint64_t> values) {
    return encodeUint64(values, impl->encodingOption);
}

IntegerEncodingResult IntegerEncoder::encodeUint64(std::span<const std::uint64_t> values,
                                                   IntegerEncodingOption option) {
    const auto signedEncoded = tryEncodeViaSignedPath<std::uint64_t, std::int64_t>(
        values, [&](auto& signedValues) { return encodeLong(signedValues, /*isSigned=*/false, option); });
    if (signedEncoded.has_value()) {
        return *signedEncoded;
    }

    const auto encode = [](std::span<const std::uint64_t> input) {
        return encodeVarintsUnsigned64(input);
    };
    return encodeUnsignedIntegralNoDelta<std::uint64_t>(values, option, encode);
}

util::EncodedChunks IntegerEncoder::buildStream(IntegerEncodingResult&& encoded,
                                                std::uint32_t totalValues,
                                                PhysicalLevelTechnique physicalTechnique,
                                                PhysicalStreamType streamType,
                                                std::optional<LogicalStreamType> logicalType) {
    const bool isRle = (encoded.logicalLevelTechnique1 == LogicalLevelTechnique::RLE ||
                        encoded.logicalLevelTechnique2 == LogicalLevelTechnique::RLE);
    std::vector<std::uint8_t> metadata;
    if (isRle) {
        metadata = RleEncodedStreamMetadata(streamType,
                                            std::move(logicalType),
                                            encoded.logicalLevelTechnique1,
                                            encoded.logicalLevelTechnique2,
                                            physicalTechnique,
                                            encoded.physicalLevelEncodedValuesLength,
                                            static_cast<std::uint32_t>(encoded.encodedValues.size()),
                                            encoded.numRuns,
                                            totalValues)
                       .encode();
    } else {
        metadata = StreamMetadata(streamType,
                                  std::move(logicalType),
                                  encoded.logicalLevelTechnique1,
                                  encoded.logicalLevelTechnique2,
                                  physicalTechnique,
                                  encoded.physicalLevelEncodedValuesLength,
                                  static_cast<std::uint32_t>(encoded.encodedValues.size()))
                       .encode();
    }

    return {std::move(metadata), std::move(encoded.encodedValues)};
}

util::EncodedChunks IntegerEncoder::encodeIntStream(std::span<const std::int32_t> values,
                                                    PhysicalLevelTechnique physicalTechnique,
                                                    bool isSigned,
                                                    PhysicalStreamType streamType,
                                                    std::optional<LogicalStreamType> logicalType) {
    return encodeIntStream(
        values, physicalTechnique, isSigned, streamType, std::move(logicalType), impl->encodingOption);
}

util::EncodedChunks IntegerEncoder::encodeIntStream(std::span<const std::int32_t> values,
                                                    PhysicalLevelTechnique physicalTechnique,
                                                    bool isSigned,
                                                    PhysicalStreamType streamType,
                                                    std::optional<LogicalStreamType> logicalType,
                                                    IntegerEncodingOption option) {
    return encodeStreamWithMetadata<std::int32_t>(
        values,
        physicalTechnique,
        streamType,
        std::move(logicalType),
        [this, physicalTechnique, isSigned, option](std::span<const std::int32_t> input) {
            return encodeInt(input, physicalTechnique, isSigned, option);
        },
        &IntegerEncoder::buildStream);
}

util::EncodedChunks IntegerEncoder::encodeLongStream(std::span<const std::int64_t> values,
                                                     bool isSigned,
                                                     PhysicalStreamType streamType,
                                                     std::optional<LogicalStreamType> logicalType) {
    return encodeLongStream(values, isSigned, streamType, std::move(logicalType), impl->encodingOption);
}

util::EncodedChunks IntegerEncoder::encodeLongStream(std::span<const std::int64_t> values,
                                                     bool isSigned,
                                                     PhysicalStreamType streamType,
                                                     std::optional<LogicalStreamType> logicalType,
                                                     IntegerEncodingOption option) {
    return encodeStreamWithMetadata<std::int64_t>(
        values,
        PhysicalLevelTechnique::VARINT,
        streamType,
        std::move(logicalType),
        [isSigned, option](std::span<const std::int64_t> input) { return encodeLong(input, isSigned, option); },
        &IntegerEncoder::buildStream);
}

util::EncodedChunks IntegerEncoder::encodeUint32Stream(std::span<const std::uint32_t> values,
                                                       PhysicalLevelTechnique physicalTechnique,
                                                       PhysicalStreamType streamType,
                                                       std::optional<LogicalStreamType> logicalType) {
    return encodeUint32Stream(values, physicalTechnique, streamType, std::move(logicalType), impl->encodingOption);
}

util::EncodedChunks IntegerEncoder::encodeUint32Stream(std::span<const std::uint32_t> values,
                                                       PhysicalLevelTechnique physicalTechnique,
                                                       PhysicalStreamType streamType,
                                                       std::optional<LogicalStreamType> logicalType,
                                                       IntegerEncodingOption option) {
    return encodeStreamWithMetadata<std::uint32_t>(
        values,
        physicalTechnique,
        streamType,
        std::move(logicalType),
        [this, physicalTechnique, option](std::span<const std::uint32_t> input) {
            return encodeUint32(input, physicalTechnique, option);
        },
        &IntegerEncoder::buildStream);
}

util::EncodedChunks IntegerEncoder::encodeUint64Stream(std::span<const std::uint64_t> values,
                                                       PhysicalStreamType streamType,
                                                       std::optional<LogicalStreamType> logicalType) {
    return encodeUint64Stream(values, streamType, std::move(logicalType), impl->encodingOption);
}

util::EncodedChunks IntegerEncoder::encodeUint64Stream(std::span<const std::uint64_t> values,
                                                       PhysicalStreamType streamType,
                                                       std::optional<LogicalStreamType> logicalType,
                                                       IntegerEncodingOption option) {
    return encodeStreamWithMetadata<std::uint64_t>(
        values,
        PhysicalLevelTechnique::VARINT,
        streamType,
        std::move(logicalType),
        [option](std::span<const std::uint64_t> input) { return encodeUint64(input, option); },
        &IntegerEncoder::buildStream);
}

} // namespace mlt::encoder
