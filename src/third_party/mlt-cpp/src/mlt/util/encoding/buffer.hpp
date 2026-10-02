#pragma once

#include <cstdint>
#include <vector>

namespace mlt::util {

using EncodedChunks = std::vector<std::vector<std::uint8_t>>;

inline EncodedChunks& appendChunks(EncodedChunks& destination, EncodedChunks&& chunks) {
    destination.insert(
        destination.end(), std::make_move_iterator(chunks.begin()), std::make_move_iterator(chunks.end()));
    return destination;
}

} // namespace mlt::util
