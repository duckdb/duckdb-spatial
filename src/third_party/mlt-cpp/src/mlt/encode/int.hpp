#pragma once

#include <mlt/metadata/stream.hpp>
#include <mlt/util/encoding/rle.hpp>
#include <mlt/util/encoding/varint.hpp>
#include <mlt/util/encoding/zigzag.hpp>
#include <mlt/util/noncopyable.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <vector>

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

    IntegerEncodingResult encodeInt(std::span<const std::int32_t> values, PhysicalLevelTechnique, bool isSigned);

    IntegerEncodingResult encodeLong(std::span<const std::int64_t> values, bool isSigned);

    std::vector<std::uint8_t> encodeIntStream(std::span<const std::int32_t> values,
                                              PhysicalLevelTechnique,
                                              bool isSigned,
                                              PhysicalStreamType,
                                              std::optional<LogicalStreamType>);

    std::vector<std::uint8_t> encodeLongStream(std::span<const std::int64_t> values,
                                               bool isSigned,
                                               PhysicalStreamType,
                                               std::optional<LogicalStreamType>);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    std::vector<std::uint8_t> encodeVarints(std::span<const std::int32_t> values, bool zigZag);
    std::vector<std::uint8_t> encodeVarints(std::span<const std::int64_t> values, bool zigZag);
    std::vector<std::uint8_t> encodeFastPfor(std::span<const std::int32_t> values, bool zigZag);

    static std::vector<std::uint8_t> buildStream(const IntegerEncodingResult& encoded,
                                                 std::uint32_t totalValues,
                                                 PhysicalLevelTechnique,
                                                 PhysicalStreamType,
                                                 std::optional<LogicalStreamType>);
};

} // namespace mlt::encoder
