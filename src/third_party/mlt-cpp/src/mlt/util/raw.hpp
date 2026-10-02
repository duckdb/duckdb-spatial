#pragma once

#include <mlt/common.hpp>
#include <mlt/metadata/stream.hpp>

#include <algorithm>
#include <cassert>

namespace mlt::util::decoding {

inline void decodeRaw(BufferStream& tileData, std::vector<std::uint8_t>& out, std::uint32_t numBytes, bool consume) {
    out.resize(numBytes);
    std::copy(tileData.getReadPosition(), tileData.getReadPosition() + numBytes, out.begin());
    if (consume) {
        tileData.consume(numBytes);
    }
}

template <typename T>
void decodeRaw(BufferStream& tileData,
               std::vector<T>& out,
               const metadata::stream::StreamMetadata& metadata,
               bool consume) {
    const auto numValues = metadata.getNumValues();
    const auto numBytes = static_cast<std::uint32_t>(sizeof(T) * numValues);
    assert(numBytes == metadata.getByteLength());
    out.resize(numValues);
    std::copy(
        tileData.getReadPosition(), tileData.getReadPosition() + numBytes, reinterpret_cast<std::uint8_t*>(out.data()));
    if (consume) {
        tileData.consume(numBytes);
    }
}

} // namespace mlt::util::decoding
