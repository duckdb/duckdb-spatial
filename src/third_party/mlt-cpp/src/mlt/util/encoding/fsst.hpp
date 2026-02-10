#pragma once

#include <fsst.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace mlt::util::encoding::fsst {

struct FsstResult {
    std::vector<std::uint8_t> symbols;
    std::vector<std::uint32_t> symbolLengths;
    std::vector<std::uint8_t> compressedData;
    std::uint32_t decompressedLength;
};

struct EncoderDeleter {
    void operator()(fsst_encoder_t* e) const { fsst_destroy(e); }
};

inline FsstResult encode(std::span<const std::uint8_t> data) {
    if (data.empty()) {
        return {{}, {}, {}, 0};
    }

    const auto* strIn = data.data();
    auto lenIn = data.size();
    std::unique_ptr<fsst_encoder_t, EncoderDeleter> encoder(fsst_create(1, &lenIn, &strIn, 0));
    if (!encoder) {
        throw std::runtime_error("fsst_create failed");
    }

    std::vector<std::uint8_t> outBuf(7 + 2 * lenIn);
    std::size_t lenOut = 0;
    unsigned char* strOut = nullptr;
    auto compressed = fsst_compress(encoder.get(), 1, &lenIn, &strIn, outBuf.size(), outBuf.data(), &lenOut, &strOut);
    if (compressed != 1) {
        throw std::runtime_error("fsst_compress failed");
    }
    outBuf.resize(lenOut);

    // Extract nSymbols from the serialized header (embedded in the version field at bits 8..15).
    std::uint8_t exportBuf[sizeof(fsst_decoder_t)];
    fsst_export(encoder.get(), exportBuf);
    std::uint64_t version;
    std::memcpy(&version, exportBuf, 8);
    auto nSymbols = static_cast<int>((version >> 8) & 0xFF);

    auto decoder = fsst_decoder(encoder.get());

    // The decoder stores symbols in length-group order (2,3,4,5,6,7,8,1) —
    // matching the MLT wire format expected by the Java/C++ decoders.
    std::vector<std::uint8_t> symbolBytes;
    std::vector<std::uint32_t> symbolLengths;
    symbolLengths.reserve(nSymbols);
    for (int i = 0; i < nSymbols; ++i) {
        auto len = decoder.len[i];
        symbolLengths.push_back(len);
        auto sym = decoder.symbol[i];
        for (unsigned j = 0; j < len; ++j) {
            symbolBytes.push_back(static_cast<std::uint8_t>(sym & 0xFF));
            sym >>= 8;
        }
    }

    return FsstResult{
        std::move(symbolBytes),
        std::move(symbolLengths),
        std::move(outBuf),
        static_cast<std::uint32_t>(lenIn),
    };
}

} // namespace mlt::util::encoding::fsst
