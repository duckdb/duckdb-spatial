#pragma once

#include <mlt/common.hpp>
#include <mlt/util/buffer_stream.hpp>
#include <mlt/util/noncopyable.hpp>
#include <mlt/util/varint.hpp>

#include <optional>
#include <memory>
#include <vector>

namespace mlt::metadata::stream {

enum class DictionaryType : std::uint32_t {
    NONE = 0,
    SINGLE = 1,
    SHARED = 2,
    VERTEX = 3,
    MORTON = 4,
    FSST = 5,
    VALUE_COUNT = 6,
};

enum class LengthType : std::uint32_t {
    VAR_BINARY = 0,
    GEOMETRIES = 1,
    PARTS = 2,
    RINGS = 3,
    TRIANGLES = 4,
    SYMBOL = 5,
    DICTIONARY = 6,
    VALUE_COUNT = 7,
};

enum class PhysicalLevelTechnique : std::uint32_t {
    NONE = 0,
    /// Preferred, tends to produce the best compression ratio and decoding performance.
    /// But currently limited to 32-bit integer.
    FAST_PFOR = 1,
    /// Can produce better results in combination with a heavyweight compression scheme like Gzip.
    /// Simple compression scheme where the decoder are easier to implement compared to FastPfor.
    VARINT = 2,
    /// Adaptive Lossless floating-Point Compression
    ALP = 3,
    VALUE_COUNT = 4,
};

enum class LogicalLevelTechnique : std::uint32_t {
    NONE = 0,
    DELTA = 1,
    COMPONENTWISE_DELTA = 2,
    RLE = 3,
    MORTON = 4,
    PSEUDODECIMAL = 5,
    VALUE_COUNT = 6,
};

enum class OffsetType : std::uint32_t {
    VERTEX = 0,
    INDEX = 1,
    STRING = 2,
    KEY = 3,
    VALUE_COUNT = 4,
};

enum class PhysicalStreamType : std::uint32_t {
    PRESENT = 0,
    DATA = 1,
    OFFSET = 2,
    LENGTH = 3,
    VALUE_COUNT = 4,
};

class LogicalStreamType : public util::noncopyable {
public:
    LogicalStreamType(DictionaryType type) noexcept
        : dictionaryType(type) {}
    LogicalStreamType(OffsetType type) noexcept
        : offsetType(type) {}
    LogicalStreamType(LengthType type) noexcept
        : lengthType(type) {}

    LogicalStreamType() = delete;
    LogicalStreamType(LogicalStreamType&&) noexcept = default;
    LogicalStreamType& operator=(LogicalStreamType&&) noexcept = default;

    const std::optional<DictionaryType>& getDictionaryType() const noexcept { return dictionaryType; }
    const std::optional<OffsetType>& getOffsetType() const noexcept { return offsetType; }
    const std::optional<LengthType>& getLengthType() const noexcept { return lengthType; }

private:
    std::optional<DictionaryType> dictionaryType;
    std::optional<OffsetType> offsetType;
    std::optional<LengthType> lengthType;
};

class StreamMetadata : public util::noncopyable {
public:
    StreamMetadata() = delete;
    StreamMetadata(PhysicalStreamType physicalStreamType_,
                   std::optional<LogicalStreamType> logicalStreamType_,
                   LogicalLevelTechnique logicalLevelTechnique1_,
                   LogicalLevelTechnique logicalLevelTechnique2_,
                   PhysicalLevelTechnique physicalLevelTechnique_,
                   std::uint32_t numValues_,
                   std::uint32_t byteLength_) noexcept
        : physicalStreamType(physicalStreamType_),
          logicalStreamType(std::move(logicalStreamType_)),
          logicalLevelTechnique1(logicalLevelTechnique1_),
          logicalLevelTechnique2(logicalLevelTechnique2_),
          physicalLevelTechnique(physicalLevelTechnique_),
          numValues(numValues_),
          byteLength(byteLength_) {}
    virtual ~StreamMetadata() noexcept = default;
    StreamMetadata(StreamMetadata&&) noexcept = default;
    StreamMetadata& operator=(StreamMetadata&&) noexcept = default;

    virtual LogicalLevelTechnique getMetadataType() const noexcept { return LogicalLevelTechnique::NONE; }

    static std::unique_ptr<StreamMetadata> decode(BufferStream&);

    virtual std::vector<std::uint8_t> encode() const;

    PhysicalStreamType getPhysicalStreamType() const { return physicalStreamType; }
    const std::optional<LogicalStreamType>& getLogicalStreamType() const { return logicalStreamType; }
    LogicalLevelTechnique getLogicalLevelTechnique1() const { return logicalLevelTechnique1; }
    LogicalLevelTechnique getLogicalLevelTechnique2() const { return logicalLevelTechnique2; }
    PhysicalLevelTechnique getPhysicalLevelTechnique() const { return physicalLevelTechnique; }

    std::uint32_t getNumValues() const noexcept { return numValues; }
    std::uint32_t getByteLength() const noexcept { return byteLength; }

private:
    int getLogicalType() const noexcept;

    friend class RleEncodedStreamMetadata;
    friend class MortonEncodedStreamMetadata;
    friend std::unique_ptr<StreamMetadata> decode(BufferStream&);
    static StreamMetadata decodeInternal(BufferStream&);

    PhysicalStreamType physicalStreamType;
    std::optional<LogicalStreamType> logicalStreamType;
    LogicalLevelTechnique logicalLevelTechnique1;
    LogicalLevelTechnique logicalLevelTechnique2;
    PhysicalLevelTechnique physicalLevelTechnique;

    // After logical Level technique was applied -> when rle is used it is the length of the runs and values array
    std::uint32_t numValues;
    std::uint32_t byteLength;
};

class RleEncodedStreamMetadata : public StreamMetadata {
public:
    /**
     * Only used for RLE encoded integer values, not boolean and byte values.
     *
     * @param numValues After LogicalLevelTechnique was applied -> numRuns + numValues
     * @param runs Length of the runs array
     * @param numRleValues Used for pre-allocating the arrays on the client for faster decoding
     */
    RleEncodedStreamMetadata(PhysicalStreamType physicalStreamType_,
                             std::optional<LogicalStreamType> logicalStreamType_,
                             LogicalLevelTechnique logicalLevelTechnique1_,
                             LogicalLevelTechnique logicalLevelTechnique2_,
                             PhysicalLevelTechnique physicalLevelTechnique_,
                             unsigned numValues_,
                             unsigned byteLength_,
                             unsigned runs_,
                             unsigned numRleValues_) noexcept
        : StreamMetadata(physicalStreamType_,
                         std::move(logicalStreamType_),
                         logicalLevelTechnique1_,
                         logicalLevelTechnique2_,
                         physicalLevelTechnique_,
                         numValues_,
                         byteLength_),
          runs(runs_),
          numRleValues(numRleValues_) {}

    RleEncodedStreamMetadata(StreamMetadata&& streamMetadata, unsigned runs_, unsigned numRleValues_) noexcept
        : StreamMetadata(std::move(streamMetadata)),
          runs(runs_),
          numRleValues(numRleValues_) {}

    RleEncodedStreamMetadata() = delete;

    LogicalLevelTechnique getMetadataType() const noexcept override { return LogicalLevelTechnique::RLE; }

    std::vector<std::uint8_t> encode() const override;

    static RleEncodedStreamMetadata decodePartial(StreamMetadata&& streamMetadata, BufferStream& tileData) {
        const auto [runs, numValues] = util::decoding::decodeVarints<std::uint32_t, 2>(tileData);
        return RleEncodedStreamMetadata(std::move(streamMetadata), runs, numValues);
    }

    static RleEncodedStreamMetadata decode(BufferStream& tileData) {
        return decodePartial(decodeInternal(tileData), tileData);
    }

    unsigned getRuns() const noexcept { return runs; }
    unsigned getNumRleValues() const noexcept { return numRleValues; }

private:
    unsigned runs;
    unsigned numRleValues;
};

class MortonEncodedStreamMetadata : public StreamMetadata {
public:
    MortonEncodedStreamMetadata(PhysicalStreamType physicalStreamType_,
                                LogicalStreamType logicalStreamType_,
                                LogicalLevelTechnique logicalLevelTechnique1_,
                                LogicalLevelTechnique logicalLevelTechnique2_,
                                PhysicalLevelTechnique physicalLevelTechnique_,
                                unsigned numValues_,
                                unsigned byteLength_,
                                unsigned numBits_,
                                unsigned coordinateShift_) noexcept
        : StreamMetadata(physicalStreamType_,
                         std::move(logicalStreamType_),
                         logicalLevelTechnique1_,
                         logicalLevelTechnique2_,
                         physicalLevelTechnique_,
                         numValues_,
                         byteLength_),
          numBits(numBits_),
          coordinateShift(coordinateShift_) {}

    MortonEncodedStreamMetadata(StreamMetadata&& streamMetadata, int numBits_, int coordinateShift_) noexcept
        : StreamMetadata(std::move(streamMetadata)),
          numBits(numBits_),
          coordinateShift(coordinateShift_) {}

    LogicalLevelTechnique getMetadataType() const noexcept override { return LogicalLevelTechnique::MORTON; }

    std::vector<std::uint8_t> encode() const override;

    static MortonEncodedStreamMetadata decodePartial(StreamMetadata&& streamMetadata, BufferStream& tileData) {
        const auto [numBits, coordShift] = util::decoding::decodeVarints<std::uint32_t, 2>(tileData);
        return MortonEncodedStreamMetadata(std::move(streamMetadata), numBits, coordShift);
    }

    static MortonEncodedStreamMetadata decode(BufferStream& tileData) {
        return decodePartial(decodeInternal(tileData), tileData);
    }

    unsigned getNumBits() const noexcept { return numBits; }
    unsigned getCoordinateShift() const noexcept { return coordinateShift; }

private:
    unsigned numBits;
    unsigned coordinateShift;
};

class PdeEncodedMetadata {};

} // namespace mlt::metadata::stream
