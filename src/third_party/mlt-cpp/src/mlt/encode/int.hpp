#pragma once

#include <mlt/metadata/stream.hpp>
#include <mlt/util/encoding/buffer.hpp>
#include <mlt/util/encoding/rle.hpp>
#include <mlt/util/encoding/varint.hpp>
#include <mlt/util/encoding/zigzag.hpp>
#include <mlt/util/noncopyable.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace mlt {
enum class IntegerEncodingOption : std::uint8_t;
} // namespace mlt

namespace mlt::encoder {

struct IntegerEncodingResult {
    metadata::stream::LogicalLevelTechnique logicalLevelTechnique1;
    metadata::stream::LogicalLevelTechnique logicalLevelTechnique2;
    std::vector<std::uint8_t> encodedValues;
    std::uint32_t numRuns = 0;
    std::uint32_t physicalLevelEncodedValuesLength = 0;
};

class IntegerEncoder : public util::noncopyable {
public:
    using PhysicalLevelTechnique = metadata::stream::PhysicalLevelTechnique;
    using LogicalLevelTechnique = metadata::stream::LogicalLevelTechnique;
    using PhysicalStreamType = metadata::stream::PhysicalStreamType;
    using LogicalStreamType = metadata::stream::LogicalStreamType;
    using StreamMetadata = metadata::stream::StreamMetadata;
    using RleEncodedStreamMetadata = metadata::stream::RleEncodedStreamMetadata;

    IntegerEncoder();
    ~IntegerEncoder() noexcept;

    IntegerEncoder(IntegerEncoder&&) = delete;
    IntegerEncoder& operator=(IntegerEncoder&&) = delete;

    void setDefaultEncodingOption(mlt::IntegerEncodingOption option);

    IntegerEncodingResult encodeInt(std::span<const std::int32_t> values, PhysicalLevelTechnique, bool isSigned);
    IntegerEncodingResult encodeInt(std::span<const std::int32_t> values,
                                    PhysicalLevelTechnique,
                                    bool isSigned,
                                    mlt::IntegerEncodingOption option);

    IntegerEncodingResult encodeLong(std::span<const std::int64_t> values, bool isSigned);
    static IntegerEncodingResult encodeLong(std::span<const std::int64_t> values,
                                            bool isSigned,
                                            mlt::IntegerEncodingOption option);

    IntegerEncodingResult encodeUint32(std::span<const std::uint32_t> values, PhysicalLevelTechnique);
    IntegerEncodingResult encodeUint32(std::span<const std::uint32_t> values,
                                       PhysicalLevelTechnique,
                                       mlt::IntegerEncodingOption option);

    IntegerEncodingResult encodeUint64(std::span<const std::uint64_t> values);
    static IntegerEncodingResult encodeUint64(std::span<const std::uint64_t> values, mlt::IntegerEncodingOption option);

    util::EncodedChunks encodeIntStream(std::span<const std::int32_t> values,
                                        PhysicalLevelTechnique,
                                        bool isSigned,
                                        PhysicalStreamType,
                                        std::optional<LogicalStreamType>);
    util::EncodedChunks encodeIntStream(std::span<const std::int32_t> values,
                                        PhysicalLevelTechnique,
                                        bool isSigned,
                                        PhysicalStreamType,
                                        std::optional<LogicalStreamType>,
                                        mlt::IntegerEncodingOption option);

    util::EncodedChunks encodeLongStream(std::span<const std::int64_t> values,
                                         bool isSigned,
                                         PhysicalStreamType,
                                         std::optional<LogicalStreamType>);
    static util::EncodedChunks encodeLongStream(std::span<const std::int64_t> values,
                                                bool isSigned,
                                                PhysicalStreamType,
                                                std::optional<LogicalStreamType>,
                                                mlt::IntegerEncodingOption option);

    util::EncodedChunks encodeUint32Stream(std::span<const std::uint32_t> values,
                                           PhysicalLevelTechnique,
                                           PhysicalStreamType,
                                           std::optional<LogicalStreamType>);
    util::EncodedChunks encodeUint32Stream(std::span<const std::uint32_t> values,
                                           PhysicalLevelTechnique,
                                           PhysicalStreamType,
                                           std::optional<LogicalStreamType>,
                                           mlt::IntegerEncodingOption option);

    util::EncodedChunks encodeUint64Stream(std::span<const std::uint64_t> values,
                                           PhysicalStreamType,
                                           std::optional<LogicalStreamType>);
    static util::EncodedChunks encodeUint64Stream(std::span<const std::uint64_t> values,
                                                  PhysicalStreamType,
                                                  std::optional<LogicalStreamType>,
                                                  mlt::IntegerEncodingOption option);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    static std::vector<std::uint8_t> encodeVarints32(std::span<const std::int32_t> values, bool zigZag);
    static std::vector<std::uint8_t> encodeVarints64(std::span<const std::int64_t> values, bool zigZag);
    static std::vector<std::uint8_t> encodeVarintsUnsigned32(std::span<const std::uint32_t> values);
    static std::vector<std::uint8_t> encodeVarintsUnsigned64(std::span<const std::uint64_t> values);
    std::vector<std::uint8_t> encodeFastPfor(std::span<const std::int32_t> values, bool zigZag);
    std::vector<std::uint8_t> encodeFastPforUnsigned(std::span<const std::uint32_t> values);

    static util::EncodedChunks buildStream(IntegerEncodingResult&& encoded,
                                           std::uint32_t totalValues,
                                           PhysicalLevelTechnique,
                                           PhysicalStreamType,
                                           std::optional<LogicalStreamType>);
};

} // namespace mlt::encoder
