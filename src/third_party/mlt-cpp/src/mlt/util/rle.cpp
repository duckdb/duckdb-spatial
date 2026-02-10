#include <mlt/util/rle.hpp>

#include <mlt/metadata/stream.hpp>

#include <cassert>

namespace mlt::util::decoding::rle {

void decodeBoolean(BufferStream& tileData,
                   std::vector<uint8_t>& out,
                   const metadata::stream::StreamMetadata& metadata,
                   bool consume) {
    const auto bitCount = metadata.getNumValues();
    const auto numBytes = (bitCount + 7) / 8;
    out.resize(numBytes);

    assert(metadata.getByteLength() <= tileData.getRemaining());
    detail::ByteRleDecoder decoder{tileData.getReadPosition(),
                                   std::min<std::size_t>(metadata.getByteLength(), tileData.getRemaining())};
    decoder.next(out.data(), numBytes);
    assert(decoder.getBufferRemaining() == 0);

    if (consume) {
        tileData.consume(metadata.getByteLength());
    }
}

} // namespace mlt::util::decoding::rle
