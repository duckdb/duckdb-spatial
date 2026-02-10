#include <mlt/decode/int.hpp>
#include <mlt/decode/int_template.hpp>

// from fastpfor/...
#if MLT_WITH_FASTPFOR
#include <compositecodec.h>
#include <fastpfor.h>
#include <variablebyte.h>
#endif // MLT_WITH_FASTPFOR

#include <cstdint>

namespace mlt::decoder {

struct IntegerDecoder::Impl {
#if MLT_WITH_FASTPFOR
    // Prevent FastPFOR from being an external dependency
    FastPForLib::CompositeCodec<FastPForLib::FastPFor<8>, FastPForLib::VariableByte> codec;
#endif // MLT_WITH_FASTPFOR
};

IntegerDecoder::IntegerDecoder()
    : impl(std::make_unique<Impl>()) {}

IntegerDecoder::~IntegerDecoder() noexcept = default;

std::uint32_t IntegerDecoder::decodeFastPfor([[maybe_unused]] BufferStream& buffer,
                                             [[maybe_unused]] std::uint32_t* const result,
                                             [[maybe_unused]] const std::size_t numValues,
                                             [[maybe_unused]] const std::size_t byteLength) {
#if MLT_WITH_FASTPFOR
    const auto* inputValues = reinterpret_cast<const std::uint32_t*>(buffer.getReadPosition());
    auto resultCount = numValues;
    impl->codec.decodeArray(inputValues, byteLength / sizeof(std::uint32_t), result, resultCount);
    return static_cast<std::uint32_t>(resultCount);
#else
    throw std::runtime_error("FastPFOR decoding is not enabled. Configure with MLT_WITH_FASTPFOR=ON");
#endif // MLT_WITH_FASTPFOR
}

} // namespace mlt::decoder
