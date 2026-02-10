#include <mlt/metadata/stream.hpp>

#include <mlt/util/encoding/varint.hpp>

#include <string>
#include <utility>

namespace mlt::metadata::stream {

namespace {
std::optional<LogicalStreamType> decodeLogicalStreamType(PhysicalStreamType physicalStreamType, int value) {
    switch (physicalStreamType) {
        case PhysicalStreamType::DATA: {
            const auto type = static_cast<DictionaryType>(value);
            if (type < DictionaryType::VALUE_COUNT) {
                return type;
            }
            break;
        }
        case PhysicalStreamType::OFFSET: {
            const auto type = static_cast<OffsetType>(value);
            if (type < OffsetType::VALUE_COUNT) {
                return type;
            }
            break;
        }
        case PhysicalStreamType::LENGTH: {
            const auto type = static_cast<LengthType>(value);
            if (type < LengthType::VALUE_COUNT) {
                return type;
            }
            break;
        }
        case PhysicalStreamType::PRESENT:
            return {};
        default:
            break;
    }
    throw std::runtime_error("Invalid logical stream type: " + std::to_string(std::to_underlying(physicalStreamType)));
}
} // namespace

std::unique_ptr<StreamMetadata> StreamMetadata::decode(BufferStream& tileData) {
    auto streamMetadata = decodeInternal(tileData);

    // Currently Morton can't be combined with RLE only with delta
    if (streamMetadata.getLogicalLevelTechnique1() == LogicalLevelTechnique::MORTON) {
        auto result = MortonEncodedStreamMetadata::decodePartial(std::move(streamMetadata), tileData);
        return std::make_unique<MortonEncodedStreamMetadata>(std::move(result));
    } else if ((streamMetadata.getLogicalLevelTechnique1() == LogicalLevelTechnique::RLE ||
                streamMetadata.getLogicalLevelTechnique2() == LogicalLevelTechnique::RLE) &&
               streamMetadata.getPhysicalLevelTechnique() != PhysicalLevelTechnique::NONE) {
        auto result = RleEncodedStreamMetadata::decodePartial(std::move(streamMetadata), tileData);
        return std::make_unique<RleEncodedStreamMetadata>(std::move(result));
    }
    return std::make_unique<StreamMetadata>(std::move(streamMetadata));
}

int StreamMetadata::getLogicalType() const noexcept {
    if (logicalStreamType) {
        if (logicalStreamType->getDictionaryType()) {
            return std::to_underlying(*logicalStreamType->getDictionaryType());
        }

        if (logicalStreamType->getLengthType()) {
            return std::to_underlying(*logicalStreamType->getLengthType());
        }

        if (logicalStreamType->getOffsetType()) {
            return std::to_underlying(*logicalStreamType->getOffsetType());
        }
    }
    return 0;
}

StreamMetadata StreamMetadata::decodeInternal(BufferStream& tileData) {
    const auto streamType = tileData.read();
    const auto physicalStreamType = static_cast<PhysicalStreamType>(streamType >> 4);
    auto logicalStreamType = decodeLogicalStreamType(physicalStreamType, streamType & 0x0f);

    const auto encodingsHeader = tileData.read() & 0xff;
    const auto logicalLevelTechnique1 = static_cast<LogicalLevelTechnique>(encodingsHeader >> 5);
    const auto logicalLevelTechnique2 = static_cast<LogicalLevelTechnique>((encodingsHeader >> 2) & 0x7);
    const auto physicalLevelTechnique = static_cast<PhysicalLevelTechnique>(encodingsHeader & 0x3);

    if (physicalStreamType >= PhysicalStreamType::VALUE_COUNT ||
        logicalLevelTechnique1 >= LogicalLevelTechnique::VALUE_COUNT ||
        logicalLevelTechnique2 >= LogicalLevelTechnique::VALUE_COUNT ||
        physicalLevelTechnique >= PhysicalLevelTechnique::VALUE_COUNT) {
        throw std::runtime_error("Invalid stream encoding");
    }

    using namespace util::decoding;
    const auto [numValues, byteLength] = decodeVarints<std::uint32_t, 2>(tileData);

    return {
        physicalStreamType,
        std::move(logicalStreamType),
        logicalLevelTechnique1,
        logicalLevelTechnique2,
        physicalLevelTechnique,
        numValues,
        byteLength,
    };
}

std::vector<std::uint8_t> StreamMetadata::encode() const {
    std::vector<std::uint8_t> result;
    result.reserve(16);

    const auto encodedStreamType = static_cast<std::uint8_t>((std::to_underlying(physicalStreamType) << 4) |
                                                             getLogicalType());
    const auto encodedEncodingScheme = static_cast<std::uint8_t>((std::to_underlying(logicalLevelTechnique1) << 5) |
                                                                 (std::to_underlying(logicalLevelTechnique2) << 2) |
                                                                 std::to_underlying(physicalLevelTechnique));

    result.push_back(encodedStreamType);
    result.push_back(encodedEncodingScheme);
    util::encoding::encodeVarint(numValues, result);
    util::encoding::encodeVarint(byteLength, result);
    return result;
}

std::vector<std::uint8_t> RleEncodedStreamMetadata::encode() const {
    auto result = StreamMetadata::encode();
    util::encoding::encodeVarint(runs, result);
    util::encoding::encodeVarint(numRleValues, result);
    return result;
}

std::vector<std::uint8_t> MortonEncodedStreamMetadata::encode() const {
    auto result = StreamMetadata::encode();
    util::encoding::encodeVarint(numBits, result);
    util::encoding::encodeVarint(coordinateShift, result);
    return result;
}

} // namespace mlt::metadata::stream
