#pragma once

#include <mlt/metadata/stream.hpp>
#include <mlt/util/encoding/rle.hpp>

#include <cstdint>
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

        std::vector<std::uint8_t> bitset;
        bitset.reserve(numBytes);

        std::uint8_t currentByte = 0;
        std::uint8_t mask = 1;
        for (const auto value : values) {
            if (static_cast<bool>(value)) {
                currentByte |= mask;
            }
            mask <<= 1;
            if (mask == 0) {
                bitset.push_back(currentByte);
                currentByte = 0;
                mask = 1;
            }
        }
        if (mask != 1) {
            bitset.push_back(currentByte);
        }

        const auto encodedData = util::encoding::rle::encodeBooleanRle(bitset.data(),
                                                                       static_cast<std::uint32_t>(count));

        const auto metadata = StreamMetadata(streamType,
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
