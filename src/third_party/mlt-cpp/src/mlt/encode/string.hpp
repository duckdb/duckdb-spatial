#pragma once

#include <mlt/encode/boolean.hpp>
#include <mlt/encode/int.hpp>
#include <mlt/metadata/stream.hpp>
#include <mlt/util/encoding/fsst.hpp>
#include <mlt/util/encoding/varint.hpp>
#include <mlt/util/stl.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mlt::encoder {

class StringEncoder {
public:
    using PhysicalLevelTechnique = metadata::stream::PhysicalLevelTechnique;
    using PhysicalStreamType = metadata::stream::PhysicalStreamType;
    using LogicalStreamType = metadata::stream::LogicalStreamType;
    using LogicalLevelTechnique = metadata::stream::LogicalLevelTechnique;
    using StreamMetadata = metadata::stream::StreamMetadata;
    using LengthType = metadata::stream::LengthType;
    using DictionaryType = metadata::stream::DictionaryType;
    using OffsetType = metadata::stream::OffsetType;
    using EncodedChunks = std::vector<std::vector<std::uint8_t>>;

    struct EncodeResult {
        std::uint32_t numStreams;
        EncodedChunks chunks;
    };

    static EncodeResult encode(std::span<const std::string_view> values,
                               PhysicalLevelTechnique physicalTechnique,
                               IntegerEncoder& intEncoder,
                               bool useFsst = true) {
        auto plain = encodeStringBytes(
            values, physicalTechnique, intEncoder, LengthType::VAR_BINARY, DictionaryType::NONE);
        auto dict = encodeDictionary(values, physicalTechnique, intEncoder);

        std::uint32_t bestStreams = 2;
        EncodedChunks* best = &plain;
        if (chunkSize(dict) < chunkSize(*best)) {
            bestStreams = 3;
            best = &dict;
        }

        if (useFsst) {
            auto fsstDict = encodeFsstDictionary(values, physicalTechnique, intEncoder, true, false);
            if (chunkSize(fsstDict) < chunkSize(*best)) {
                return {.numStreams = 5, .chunks = std::move(fsstDict)};
            }
        }

        return {.numStreams = bestStreams, .chunks = std::move(*best)};
    }

    static EncodeResult encodeSharedDictionary(const std::vector<std::vector<std::optional<std::string_view>>>& columns,
                                               PhysicalLevelTechnique physicalTechnique,
                                               IntegerEncoder& intEncoder,
                                               bool useFsst = true) {
        std::vector<std::string_view> dictionary;
        std::unordered_map<std::string_view, std::uint32_t> dictIndex;
        std::vector<std::vector<std::int32_t>> dataStreams(columns.size());
        std::vector<std::vector<bool>> presentStreams(columns.size());

        for (std::size_t col = 0; col < columns.size(); ++col) {
            for (const auto& optSv : columns[col]) {
                if (!optSv) {
                    presentStreams[col].push_back(false);
                } else {
                    presentStreams[col].push_back(true);
                    auto [it, inserted] = dictIndex.try_emplace(*optSv, static_cast<std::uint32_t>(dictionary.size()));
                    if (inserted) {
                        dictionary.push_back(*optSv);
                    }
                    dataStreams[col].push_back(static_cast<std::int32_t>(it->second));
                }
            }
        }

        if (dictionary.empty()) {
            return {.numStreams = 0, .chunks = {}};
        }

        auto dictData = encodeStringBytes(
            dictionary, physicalTechnique, intEncoder, LengthType::DICTIONARY, DictionaryType::SHARED);
        // Non-FSST shared dictionary emits only LENGTH + DATA streams.
        std::uint32_t dictStreams = 2;

        if (useFsst) {
            auto fsstData = encodeFsst(dictionary, physicalTechnique, intEncoder, true);
            if (chunkSize(fsstData) < chunkSize(dictData)) {
                dictData = std::move(fsstData);
                // FSST shared dictionary emits LENGTH(symbol), DATA(symbol table),
                // LENGTH(dictionary), DATA(compressed dictionary).
                dictStreams = 4;
            }
        }

        EncodedChunks chunks;
        chunks.reserve(1 + (columns.size() * 3));
        util::appendChunks(chunks, std::move(dictData));

        for (std::size_t col = 0; col < columns.size(); ++col) {
            if (dataStreams[col].empty()) {
                std::vector<std::uint8_t> emptyColumn;
                util::encoding::encodeVarint(static_cast<std::uint32_t>(0), emptyColumn);
                chunks.push_back(std::move(emptyColumn));
                continue;
            }

            std::vector<std::uint8_t> columnNumStreams;
            util::encoding::encodeVarint(static_cast<std::uint32_t>(2), columnNumStreams);
            chunks.push_back(std::move(columnNumStreams));

            auto presentData = BooleanEncoder::encodeBooleanStream(presentStreams[col], PhysicalStreamType::PRESENT);
            chunks.push_back(std::move(presentData));

            util::appendChunks(chunks,
                               intEncoder.encodeIntStream(dataStreams[col],
                                                          physicalTechnique,
                                                          false,
                                                          PhysicalStreamType::OFFSET,
                                                          LogicalStreamType{OffsetType::STRING}));
        }

        const auto numStreams = dictStreams + (static_cast<std::uint32_t>(columns.size()) * 2);
        return {.numStreams = numStreams, .chunks = std::move(chunks)};
    }

private:
    struct EncodedStringStreams {
        EncodedChunks firstStreamChunks;
        EncodedChunks remainingStreamChunks;
    };

    struct DictionaryIndex {
        std::vector<std::string_view> dictionary;
        std::vector<std::int32_t> offsets;
    };

    static DictionaryIndex buildDictIndex(std::span<const std::string_view> values) {
        DictionaryIndex result;
        result.offsets.reserve(values.size());
        std::unordered_map<std::string_view, std::uint32_t> indexMap;
        indexMap.reserve(values.size());
        for (const auto sv : values) {
            auto [it, inserted] = indexMap.try_emplace(sv, static_cast<std::uint32_t>(result.dictionary.size()));
            if (inserted) {
                result.dictionary.push_back(sv);
            }
            result.offsets.push_back(static_cast<std::int32_t>(it->second));
        }
        return result;
    }

    static std::size_t chunkSize(const EncodedChunks& chunks) {
        return util::sum(chunks, [](const auto& chunk) { return chunk.size(); });
    }

    static EncodedChunks encodeOffsetsAndData(EncodedChunks firstStreamChunks,
                                              EncodedChunks remainingStreamChunks,
                                              std::span<const std::int32_t> offsets,
                                              PhysicalLevelTechnique physicalTechnique,
                                              IntegerEncoder& intEncoder) {
        EncodedChunks result;
        result.reserve(firstStreamChunks.size() + remainingStreamChunks.size() + 2);
        util::appendChunks(result, std::move(firstStreamChunks));
        util::appendChunks(
            result,
            intEncoder.encodeIntStream(
                offsets, physicalTechnique, false, PhysicalStreamType::OFFSET, LogicalStreamType{OffsetType::STRING}));
        util::appendChunks(result, std::move(remainingStreamChunks));
        return result;
    }

    static EncodedChunks encodeDictionary(std::span<const std::string_view> values,
                                          PhysicalLevelTechnique physicalTechnique,
                                          IntegerEncoder& intEncoder) {
        const auto [dictionary, offsets] = buildDictIndex(values);
        auto dictStreams = encodeStringStreams(
            dictionary, physicalTechnique, intEncoder, LengthType::DICTIONARY, DictionaryType::SINGLE);
        return encodeOffsetsAndData(std::move(dictStreams.firstStreamChunks),
                                    std::move(dictStreams.remainingStreamChunks),
                                    offsets,
                                    physicalTechnique,
                                    intEncoder);
    }

    static EncodedChunks encodeFsstDictionary(std::span<const std::string_view> values,
                                              PhysicalLevelTechnique physicalTechnique,
                                              IntegerEncoder& intEncoder,
                                              bool encodeOffsets,
                                              bool isShared) {
        const auto [dictionary, offsets] = buildDictIndex(values);
        auto fsstData = encodeFsst(dictionary, physicalTechnique, intEncoder, isShared);

        if (!encodeOffsets) {
            return fsstData;
        }

        auto fsstStreams = encodeFsstStreams(dictionary, physicalTechnique, intEncoder, isShared);
        return encodeOffsetsAndData(std::move(fsstStreams.firstStreamChunks),
                                    std::move(fsstStreams.remainingStreamChunks),
                                    offsets,
                                    physicalTechnique,
                                    intEncoder);
    }

    static EncodedChunks encodeFsst(std::span<const std::string_view> values,
                                    PhysicalLevelTechnique physicalTechnique,
                                    IntegerEncoder& intEncoder,
                                    bool isShared) {
        auto streams = encodeFsstStreams(values, physicalTechnique, intEncoder, isShared);
        EncodedChunks result;
        result.reserve(streams.firstStreamChunks.size() + streams.remainingStreamChunks.size());
        util::appendChunks(result, std::move(streams.firstStreamChunks));
        util::appendChunks(result, std::move(streams.remainingStreamChunks));
        return result;
    }

    static EncodedStringStreams encodeFsstStreams(std::span<const std::string_view> values,
                                                  PhysicalLevelTechnique physicalTechnique,
                                                  IntegerEncoder& intEncoder,
                                                  bool isShared) {
        namespace fsst = util::encoding::fsst;

        std::vector<std::int32_t> valueLengths;
        valueLengths.reserve(values.size());
        std::size_t totalBytes = 0;
        for (const auto sv : values) {
            valueLengths.push_back(static_cast<std::int32_t>(sv.size()));
            totalBytes += sv.size();
        }
        std::vector<std::uint8_t> joined;
        joined.reserve(totalBytes);
        for (const auto sv : values) {
            joined.insert(joined.end(), sv.begin(), sv.end());
        }

        const auto result = fsst::encode(joined);

        std::vector<std::int32_t> symLens(result.symbolLengths.begin(), result.symbolLengths.end());
        auto encodedSymbolLengths = intEncoder.encodeIntStream(
            symLens, physicalTechnique, false, PhysicalStreamType::LENGTH, LogicalStreamType{LengthType::SYMBOL});

        auto symbolTableMetadata = StreamMetadata(PhysicalStreamType::DATA,
                                                  LogicalStreamType{DictionaryType::FSST},
                                                  LogicalLevelTechnique::NONE,
                                                  LogicalLevelTechnique::NONE,
                                                  PhysicalLevelTechnique::NONE,
                                                  static_cast<std::uint32_t>(result.symbolLengths.size()),
                                                  static_cast<std::uint32_t>(result.symbols.size()))
                                       .encode();

        auto encodedDictLengths = intEncoder.encodeIntStream(valueLengths,
                                                             physicalTechnique,
                                                             false,
                                                             PhysicalStreamType::LENGTH,
                                                             LogicalStreamType{LengthType::DICTIONARY});

        const auto dictType = isShared ? DictionaryType::SHARED : DictionaryType::SINGLE;
        auto corpusMetadata = StreamMetadata(PhysicalStreamType::DATA,
                                             LogicalStreamType{dictType},
                                             LogicalLevelTechnique::NONE,
                                             LogicalLevelTechnique::NONE,
                                             PhysicalLevelTechnique::NONE,
                                             static_cast<std::uint32_t>(values.size()),
                                             static_cast<std::uint32_t>(result.compressedData.size()))
                                  .encode();

        EncodedChunks remainder;
        remainder.reserve(4 + encodedDictLengths.size());
        remainder.push_back(std::move(symbolTableMetadata));
        remainder.push_back(result.symbols);
        util::appendChunks(remainder, std::move(encodedDictLengths));
        remainder.push_back(std::move(corpusMetadata));
        remainder.push_back(result.compressedData);

        return {
            .firstStreamChunks = std::move(encodedSymbolLengths),
            .remainingStreamChunks = std::move(remainder),
        };
    }

    static EncodedStringStreams encodeStringStreams(std::span<const std::string_view> values,
                                                    PhysicalLevelTechnique physicalTechnique,
                                                    IntegerEncoder& intEncoder,
                                                    LengthType lengthType,
                                                    DictionaryType dictType) {
        std::vector<std::int32_t> lengths;
        lengths.reserve(values.size());

        const auto totalBytes = util::sum(values, [](const auto& sv) { return sv.size(); });

        std::vector<std::uint8_t> rawData;
        rawData.reserve(totalBytes);
        for (const auto sv : values) {
            lengths.push_back(static_cast<std::int32_t>(sv.size()));
            rawData.insert(rawData.end(), sv.begin(), sv.end());
        }

        auto encodedLengths = intEncoder.encodeIntStream(
            lengths, physicalTechnique, false, PhysicalStreamType::LENGTH, LogicalStreamType{lengthType});

        auto dataMetadata = StreamMetadata(PhysicalStreamType::DATA,
                                           LogicalStreamType{dictType},
                                           LogicalLevelTechnique::NONE,
                                           LogicalLevelTechnique::NONE,
                                           PhysicalLevelTechnique::NONE,
                                           static_cast<std::uint32_t>(values.size()),
                                           static_cast<std::uint32_t>(rawData.size()))
                                .encode();

        EncodedChunks remaining;
        remaining.reserve(2);
        remaining.push_back(std::move(dataMetadata));
        remaining.push_back(std::move(rawData));

        return {
            .firstStreamChunks = std::move(encodedLengths),
            .remainingStreamChunks = std::move(remaining),
        };
    }

    static EncodedChunks encodeStringBytes(std::span<const std::string_view> values,
                                           PhysicalLevelTechnique physicalTechnique,
                                           IntegerEncoder& intEncoder,
                                           LengthType lengthType,
                                           DictionaryType dictType) {
        auto streams = encodeStringStreams(values, physicalTechnique, intEncoder, lengthType, dictType);
        EncodedChunks result;
        result.reserve(streams.firstStreamChunks.size() + streams.remainingStreamChunks.size());
        util::appendChunks(result, std::move(streams.firstStreamChunks));
        util::appendChunks(result, std::move(streams.remainingStreamChunks));
        return result;
    }
};

} // namespace mlt::encoder
