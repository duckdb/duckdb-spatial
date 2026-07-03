#include <mlt/decode/int.hpp>
#include <mlt/polyfill.hpp> // NOLINT(misc-include-cleaner)
#include <mlt/util/buffer_stream.hpp>

// from fastpfor/...
#if MLT_WITH_FASTPFOR
#include <compositecodec.h>
#include <fastpfor.h>
#include <variablebyte.h>
#endif // MLT_WITH_FASTPFOR

#ifdef _MSC_VER
#include <array>
#include <bitset>
#include <intrin.h>
#endif

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>

namespace mlt::decoder {

struct IntegerDecoder::Impl {
#if MLT_WITH_FASTPFOR
    // Impl pattern to prevent FastPFOR from being an API dependency
    FastPForLib::CompositeCodec<FastPForLib::FastPFor<8>, FastPForLib::VariableByte> codec;
#endif // MLT_WITH_FASTPFOR
};

IntegerDecoder::IntegerDecoder([[maybe_unused]] bool enableFastPFOR)
    : impl(std::make_unique<Impl>())
#if MLT_WITH_FASTPFOR
      ,
      enableFastPFOR(enableFastPFOR)
#endif
{
}

IntegerDecoder::~IntegerDecoder() noexcept = default;

std::uint32_t IntegerDecoder::decodeFastPfor([[maybe_unused]] BufferStream& buffer,
                                             [[maybe_unused]] std::uint32_t* const result,
                                             [[maybe_unused]] const std::size_t numValues,
                                             [[maybe_unused]] const std::size_t byteLength) {
#if MLT_WITH_FASTPFOR
    if (enableFastPFOR) {
        // If SIMD is enabled, check that the CPU supports the necessary instruction set before
        // attempting to decode in order to provide a clear error message when it does not.
#if MLT_WITH_FASTPFOR_SIMD
#if (defined(_M_IX86) || defined(_M_X64) || defined(_M_IA64) || defined(_M_AMD64))
#if defined(__GNUC__) || defined(__clang__)
        // https://gcc.gnu.org/onlinedocs/gcc/x86-Built-in-Functions.html
        if (!__builtin_cpu_supports("sse4.1")) {
            // The x86 implementation in FastPFOR requires SSE4.1
            throw std::runtime_error("FastPFOR decoding requires SSE4.1 on x86 platforms");
        }
#elif defined(_MSC_VER)
        // https://learn.microsoft.com/en-us/cpp/intrinsics/cpuid-cpuidex
        {
            int cpui[4];
            __cpuid(cpui, 0);
            cpui[2] = 0;
            if (cpui[0] > 0) {
                __cpuidex(cpui, 1, 0);
            }

            if (const std::bitset<32> fn1ECX = cpui[2]; !fn1ECX[19]) {
                // The x86 implementation in FastPFOR requires SSE4.1
                throw std::runtime_error("FastPFOR decoding requires SSE4.1 on x86 platforms");
            }
        }
#endif // __GNUC__ ...
#endif // _M_IX86 ...
#endif // MLT_WITH_FASTPFOR_SIMD

        const auto* inputValues = reinterpret_cast<const std::uint32_t*>(buffer.getReadPosition());

        // TODO: change to little endian in the encoder?
        const auto intLength = (byteLength + sizeof(std::uint32_t) - 1) / sizeof(std::uint32_t);
        const auto leBuffer = getTempBuffer<std::uint32_t>(intLength);
        std::transform(inputValues, inputValues + intLength, leBuffer.get(), [](std::uint32_t v) noexcept {
            return std::byteswap(v);
        });

        auto resultCount = numValues;
        impl->codec.decodeArray(leBuffer, intLength, result, resultCount);
        buffer.consume(byteLength);
        return static_cast<std::uint32_t>(resultCount);
    }
    throw std::runtime_error("FastPFOR decoding is not enabled");
#else
    throw std::runtime_error("FastPFOR decoding is not enabled. Configure with MLT_WITH_FASTPFOR=ON");
#endif // MLT_WITH_FASTPFOR
}

} // namespace mlt::decoder
