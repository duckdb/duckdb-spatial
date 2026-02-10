#pragma once

#include <mlt/metadata/stream.hpp>
#include <mlt/util/encoding/rle.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace mlt::encoder {

class BooleanEncoder {
public:
    using PhysicalStreamType = metadata::stream::PhysicalStreamType;
    using StreamMetadata = metadata::stream::StreamMetadata;

    template <typename Container>
    static std::vector<std::uint8_t> encodeBooleanStream(const Container& values, PhysicalStreamType streamType) {
        const auto count = values.size();
        const auto numBytes = (count + 7) / 8;
        std::vector<std::uint8_t> bitset(numBytes, 0);
        for (std::size_t i = 0; i < count; ++i) {
            if (values[i]) {
                bitset[i / 8] |= (1 << (i % 8));
            }
        }

        auto encodedData = util::encoding::rle::encodeBooleanRle(bitset.data(), static_cast<std::uint32_t>(count));

        auto metadata = StreamMetadata(streamType,
                                       std::nullopt,
                                       metadata::stream::LogicalLevelTechnique::RLE,
                                       metadata::stream::LogicalLevelTechnique::NONE,
                                       metadata::stream::PhysicalLevelTechnique::NONE,
                                       static_cast<std::uint32_t>(count),
                                       static_cast<std::uint32_t>(encodedData.size()))
                            .encode();

        std::vector<std::uint8_t> result;
        result.reserve(metadata.size() + encodedData.size());
        result.insert(result.end(), metadata.begin(), metadata.end());
        result.insert(result.end(), encodedData.begin(), encodedData.end());
        return result;
    }
};

} // namespace mlt::encoder
