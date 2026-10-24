#include "pdfRasterizer.h"
#include "cbzHandler.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace {

struct HuffTable {
    static constexpr int kMaxBits = 15;
    static constexpr int kMaxSyms = 288;
    static constexpr int kFastBits = 9;
    uint16_t fast[1 << kFastBits];
    int16_t count[kMaxBits + 1];
    int32_t firstCode[kMaxBits + 1];
    int16_t firstIdx[kMaxBits + 1];
    int16_t syms[kMaxSyms];

    bool build(const uint8_t *lengths, int numSyms) {
        memset(fast, 0, sizeof(fast));
        memset(count, 0, sizeof(count));
        for (int i = 0; i < numSyms; ++i) {
            if (lengths[i] > kMaxBits) return false;
            count[lengths[i]]++;
        }
        if (count[0] == numSyms) {
            return true;
        }
        int32_t code = 0;
        int idx = 0;
        for (int bits = 1; bits <= kMaxBits; ++bits) {
            firstCode[bits] = code;
            firstIdx[bits] = static_cast<int16_t>(idx);
            code = (code + count[bits]) << 1;
            idx += count[bits];
        }
        if (code > (1 << (kMaxBits + 1))) {
            return false;
        }
        int16_t nextIdx[kMaxBits + 1];
        int32_t nextCode[kMaxBits + 1];
        for (int bits = 1; bits <= kMaxBits; ++bits) {
            nextIdx[bits] = firstIdx[bits];
            nextCode[bits] = firstCode[bits];
        }
        for (int sym = 0; sym < numSyms; ++sym) {
            int len = lengths[sym];
            if (len == 0) continue;
            syms[nextIdx[len]++] = static_cast<int16_t>(sym);
            int32_t c = nextCode[len]++;
            if (len <= kFastBits) {
                int rev = 0;
                for (int b = 0; b < len; ++b) {
                    rev = (rev << 1) | ((c >> b) & 1);
                }
                uint16_t entry = static_cast<uint16_t>((len << 9) | sym);
                int step = 1 << len;
                for (int fill = rev; fill < (1 << kFastBits); fill += step) {
                    fast[fill] = entry;
                }
            }
        }
        return true;
    }
};

// Self-contained RFC 1951 (raw deflate) + RFC 1950 (zlib wrapper) decompressor.
// Uses only PSRAM (allocComicPageBuffer) and zero external library dependencies.
static uint8_t *tryZlibInflate(const uint8_t *src, size_t srcLen, int windowBits, size_t &outLen) {
    outLen = 0;
    if (!src || srcLen == 0) return nullptr;

    size_t bytePos = 0;
    if (windowBits > 0) {
        if (srcLen < 2) return nullptr;
        uint8_t cmf = src[0];
        uint8_t flg = src[1];
        if ((cmf & 0x0F) != 8 || (cmf >> 4) > 7) return nullptr;
        if ((((static_cast<uint16_t>(cmf) << 8) | flg) % 31) != 0) return nullptr;
        bytePos = 2;
        if (flg & 0x20) {
            if (srcLen < 6) return nullptr;
            bytePos = 6;
        }
    }

    constexpr size_t kMaxDecompressedBytes = 16 * 1024 * 1024;
    size_t cap = std::max<size_t>(4096, std::min<size_t>(srcLen * 3, 2 * 1024 * 1024));
    uint8_t *outBuf = allocComicPageBuffer(cap);
    if (!outBuf) return nullptr;
    size_t outPos = 0;

    auto ensureCap = [&](size_t needed) -> bool {
        if (needed <= cap) return true;
        if (needed > kMaxDecompressedBytes) return false;
        size_t newCap = cap;
        while (newCap < needed) {
            if (newCap < 1024 * 1024) {
                newCap *= 2;
            } else {
                newCap += 512 * 1024;
            }
            if (newCap > kMaxDecompressedBytes) {
                newCap = kMaxDecompressedBytes;
                break;
            }
        }
        uint8_t *reBuf = static_cast<uint8_t *>(realloc(outBuf, newCap));
        if (reBuf) {
            outBuf = reBuf;
            cap = newCap;
            return true;
        }
        uint8_t *newBuf = allocComicPageBuffer(newCap);
        if (!newBuf) return false;
        if (outPos > 0) {
            memcpy(newBuf, outBuf, outPos);
        }
        free(outBuf);
        outBuf = newBuf;
        cap = newCap;
        return true;
    };

    uint32_t bitBuf = 0;
    int bitCount = 0;

    auto ensureBits = [&](int n) {
        while (bitCount < n && bytePos < srcLen) {
            bitBuf |= static_cast<uint32_t>(src[bytePos++]) << bitCount;
            bitCount += 8;
        }
    };

    auto readBits = [&](int n) -> int {
        if (n == 0) return 0;
        ensureBits(n);
        if (bitCount < n) return -1;
        int val = static_cast<int>(bitBuf & ((1U << n) - 1U));
        bitBuf >>= n;
        bitCount -= n;
        return val;
    };

    auto decodeSym = [&](const HuffTable &ht) -> int {
        ensureBits(15);
        uint16_t f = ht.fast[bitBuf & ((1 << HuffTable::kFastBits) - 1)];
        if (f != 0) {
            int len = f >> 9;
            if (len > bitCount) return -1;
            bitBuf >>= len;
            bitCount -= len;
            return f & 0x1FF;
        }
        int32_t code = 0;
        for (int bits = 1; bits <= HuffTable::kMaxBits; ++bits) {
            if (bits > bitCount) return -1;
            code = (code << 1) | static_cast<int32_t>((bitBuf >> (bits - 1)) & 1);
            int32_t first = ht.firstCode[bits];
            int32_t off = code - first;
            if (off >= 0 && off < ht.count[bits]) {
                bitBuf >>= bits;
                bitCount -= bits;
                return ht.syms[ht.firstIdx[bits] + off];
            }
        }
        return -1;
    };

    static const uint16_t kLenBase[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
    };
    static const uint8_t kLenExtra[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
    };
    static const uint16_t kDistBase[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
    };
    static const uint8_t kDistExtra[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
    };
    static const uint8_t kCodeLenOrder[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };

    // Allocate Huffman tables on the heap (~5.1 KB) to protect the FreeRTOS main_task stack
    HuffTable *tables = reinterpret_cast<HuffTable *>(allocComicPageBuffer(sizeof(HuffTable) * 3));
    if (!tables) {
        free(outBuf);
        return nullptr;
    }
    HuffTable &litLenTable = tables[0];
    HuffTable &distTable = tables[1];
    HuffTable &codeLenTable = tables[2];

    bool ok = true;
    int bfinal = 0;
    while (!bfinal && ok) {
        bfinal = readBits(1);
        int btype = readBits(2);
        if (bfinal < 0 || btype < 0 || btype == 3) {
            ok = false;
            break;
        }

        if (btype == 0) {
            // Uncompressed stored block
            int drop = bitCount & 7;
            bitBuf >>= drop;
            bitCount -= drop;
            ensureBits(32);
            if (bitCount < 32) {
                ok = false;
                break;
            }
            uint16_t bLen = static_cast<uint16_t>(bitBuf & 0xFFFF);
            uint16_t bNlen = static_cast<uint16_t>((bitBuf >> 16) & 0xFFFF);
            bitBuf = 0;
            bitCount = 0;
            if (bLen != static_cast<uint16_t>(~bNlen)) {
                ok = false;
                break;
            }
            if (bytePos + bLen > srcLen || !ensureCap(outPos + bLen)) {
                ok = false;
                break;
            }
            if (bLen > 0) {
                memcpy(outBuf + outPos, src + bytePos, bLen);
                outPos += bLen;
                bytePos += bLen;
            }
            continue;
        }

        if (btype == 1) {
            // Fixed Huffman tables
            uint8_t fixedLit[288];
            for (int i = 0; i <= 143; ++i) fixedLit[i] = 8;
            for (int i = 144; i <= 255; ++i) fixedLit[i] = 9;
            for (int i = 256; i <= 279; ++i) fixedLit[i] = 7;
            for (int i = 280; i <= 287; ++i) fixedLit[i] = 8;
            uint8_t fixedDist[32];
            for (int i = 0; i < 32; ++i) fixedDist[i] = 5;
            if (!litLenTable.build(fixedLit, 288) || !distTable.build(fixedDist, 32)) {
                ok = false;
                break;
            }
        } else {
            // Dynamic Huffman tables
            int hlit = readBits(5);
            int hdist = readBits(5);
            int hclen = readBits(4);
            if (hlit < 0 || hdist < 0 || hclen < 0) {
                ok = false;
                break;
            }
            hlit += 257;
            hdist += 1;
            hclen += 4;
            if (hlit > 286 || hdist > 30) {
                ok = false;
                break;
            }

            uint8_t clLengths[19];
            memset(clLengths, 0, sizeof(clLengths));
            for (int i = 0; i < hclen; ++i) {
                int v = readBits(3);
                if (v < 0) { ok = false; break; }
                clLengths[kCodeLenOrder[i]] = static_cast<uint8_t>(v);
            }
            if (!ok || !codeLenTable.build(clLengths, 19)) {
                ok = false;
                break;
            }

            uint8_t lengths[320];
            memset(lengths, 0, sizeof(lengths));
            int totalSyms = hlit + hdist;
            int idx = 0;
            while (idx < totalSyms) {
                int sym = decodeSym(codeLenTable);
                if (sym < 0) { ok = false; break; }
                if (sym <= 15) {
                    lengths[idx++] = static_cast<uint8_t>(sym);
                } else if (sym == 16) {
                    int rep = readBits(2);
                    if (rep < 0 || idx == 0) { ok = false; break; }
                    rep += 3;
                    uint8_t prev = lengths[idx - 1];
                    while (rep-- > 0 && idx < totalSyms) lengths[idx++] = prev;
                } else if (sym == 17) {
                    int rep = readBits(3);
                    if (rep < 0) { ok = false; break; }
                    rep += 3;
                    while (rep-- > 0 && idx < totalSyms) lengths[idx++] = 0;
                } else if (sym == 18) {
                    int rep = readBits(7);
                    if (rep < 0) { ok = false; break; }
                    rep += 11;
                    while (rep-- > 0 && idx < totalSyms) lengths[idx++] = 0;
                } else {
                    ok = false;
                    break;
                }
            }
            if (!ok ||
                !litLenTable.build(lengths, hlit) ||
                !distTable.build(lengths + hlit, hdist))
            {
                ok = false;
                break;
            }
        }

        // Decode compressed block symbols
        while (true) {
            int sym = decodeSym(litLenTable);
            if (sym < 0) {
                // Allow slightly truncated final block if we already produced output and hit EOF
                if (bytePos >= srcLen && outPos > 0) {
                    bfinal = 1;
                } else {
                    ok = false;
                }
                break;
            }
            if (sym < 256) {
                if (outPos >= cap && !ensureCap(outPos + 1)) {
                    ok = false;
                    break;
                }
                outBuf[outPos++] = static_cast<uint8_t>(sym);
            } else if (sym == 256) {
                break; // End of block
            } else if (sym <= 285) {
                int lenIdx = sym - 257;
                int extraL = readBits(kLenExtra[lenIdx]);
                if (extraL < 0) { ok = false; break; }
                size_t matchLen = static_cast<size_t>(kLenBase[lenIdx] + extraL);

                int distSym = decodeSym(distTable);
                if (distSym < 0 || distSym >= 30) { ok = false; break; }
                int extraD = readBits(kDistExtra[distSym]);
                if (extraD < 0) { ok = false; break; }
                size_t matchDist = static_cast<size_t>(kDistBase[distSym] + extraD);

                if (matchDist == 0 || matchDist > outPos || !ensureCap(outPos + matchLen)) {
                    ok = false;
                    break;
                }
                uint8_t *dst = outBuf + outPos;
                const uint8_t *from = dst - matchDist;
                for (size_t i = 0; i < matchLen; ++i) {
                    dst[i] = from[i];
                }
                outPos += matchLen;
            } else {
                ok = false;
                break;
            }
        }
    }

    free(tables);
    if (ok && outPos > 0) {
        outLen = outPos;
        return outBuf;
    }

    free(outBuf);
    return nullptr;
}

static inline uint16_t readU16BE(const uint8_t *p) {
    return (static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]);
}

static inline int16_t readS16BE(const uint8_t *p) {
    return static_cast<int16_t>(readU16BE(p));
}

static inline uint32_t readU32BE(const uint8_t *p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |
           static_cast<uint32_t>(p[3]);
}

static inline int hexDigitVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

// =============================================================================
// 1. PDF Stream Filter Decoders
// =============================================================================

uint8_t *inflatePdfDeflateToPsram(const uint8_t *src, size_t srcLen, size_t &outLen) {
    outLen = 0;
    if (!src || srcLen == 0) return nullptr;

    // Trim trailing CR/LF/space padding that PDF writers sometimes include in /Length
    size_t trimmedLen = srcLen;
    while (trimmedLen > 2 && (src[trimmedLen - 1] == '\n' || src[trimmedLen - 1] == '\r' || src[trimmedLen - 1] == ' ')) {
        trimmedLen--;
    }

    // 1. Standard zlib wrapper (RFC 1950)
    uint8_t *p = tryZlibInflate(src, srcLen, 15, outLen);
    if (!p && trimmedLen != srcLen) {
        p = tryZlibInflate(src, trimmedLen, 15, outLen);
    }
    // Check if 1..8 leading whitespace bytes precede a valid RFC 1950 zlib header
    for (size_t skip = 1; !p && skip <= 8 && skip + 2 < trimmedLen; ++skip) {
        uint8_t prev = src[skip - 1];
        if (prev != '\r' && prev != '\n' && prev != ' ' && prev != '\t' && prev != '\0') break;
        uint8_t cmf = src[skip];
        uint8_t flg = src[skip + 1];
        if ((cmf & 0x0F) == 8 && (cmf >> 4) <= 7 && (((static_cast<uint16_t>(cmf) << 8) | flg) % 31) == 0) {
            p = tryZlibInflate(src + skip, trimmedLen - skip, 15, outLen);
        }
    }
    // 2. Raw deflate without 2-byte zlib header (RFC 1951)
    if (!p) {
        p = tryZlibInflate(src, srcLen, -15, outLen);
    }
    if (!p && trimmedLen != srcLen) {
        p = tryZlibInflate(src, trimmedLen, -15, outLen);
    }
    // 3. Fallback for streams with a non-standard/corrupted 2-byte header or truncated Adler-32 trailer
    if (!p && trimmedLen > 2) {
        p = tryZlibInflate(src + 2, trimmedLen - 2, -15, outLen);
    }
    return p;
}

static inline uint8_t flatePaethPredictor(int a, int b, int c) {
    int p = a + b - c;
    int pa = std::abs(p - a);
    int pb = std::abs(p - b);
    int pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return static_cast<uint8_t>(a);
    if (pb <= pc) return static_cast<uint8_t>(b);
    return static_cast<uint8_t>(c);
}

static bool tryStreamingFlateImageToGray(
    const uint8_t *src,
    size_t srcLen,
    int windowBits,
    int w,
    int h,
    int bpc,
    int comps,
    int predictor,
    int predictorCols,
    bool invertBits,
    const std::vector<uint8_t> &paletteGray,
    uint8_t *&outGrayBuf,
    int &outW,
    int &outH)
{
    outGrayBuf = nullptr;
    outW = outH = 0;
    if (!src || srcLen == 0 || w <= 0 || h <= 0) return false;

    size_t bytePos = 0;
    if (windowBits > 0) {
        if (srcLen < 2) return false;
        uint8_t cmf = src[0];
        uint8_t flg = src[1];
        if ((cmf & 0x0F) != 8 || (cmf >> 4) > 7) return false;
        if ((((static_cast<uint16_t>(cmf) << 8) | flg) % 31) != 0) return false;
        bytePos = 2;
        if (flg & 0x20) {
            if (srcLen < 6) return false;
            bytePos = 6;
        }
    }

    int cols = (predictorCols > 0) ? predictorCols : w;
    int effComps = (!paletteGray.empty()) ? 1 : std::max(1, comps);
    int effBpc = (bpc > 0) ? bpc : 8;
    int rowBytes = (cols * effComps * effBpc + 7) / 8;
    int bpp = std::max(1, (effComps * effBpc + 7) / 8);
    if (rowBytes <= 0 || rowBytes > 65536) return false;

    bool hasPngTag = (predictor >= 10);
    size_t stride = static_cast<size_t>(rowBytes) + (hasPngTag ? 1 : 0);

    // Downscale images larger than 1296px during streaming so peak PSRAM stays small
    int scaleShift = 0;
    while (((w >> scaleShift) > 1296 || (h >> scaleShift) > 1296) && scaleShift < 3) {
        scaleShift++;
    }
    int dstW = std::max(1, w >> scaleShift);
    int dstH = std::max(1, h >> scaleShift);

    uint8_t *grayBuf = allocComicPageBuffer(static_cast<size_t>(dstW) * dstH);
    if (!grayBuf) return false;
    memset(grayBuf, 255, static_cast<size_t>(dstW) * dstH);

    constexpr size_t kWinSize = 32768;
    constexpr size_t kWinMask = kWinSize - 1;
    uint8_t *winBuf = allocComicPageBuffer(kWinSize);
    uint8_t *rowAlloc = allocComicPageBuffer(stride + static_cast<size_t>(rowBytes) * 2);
    HuffTable *tables = reinterpret_cast<HuffTable *>(allocComicPageBuffer(sizeof(HuffTable) * 3));
    if (!winBuf || !rowAlloc || !tables) {
        if (winBuf) free(winBuf);
        if (rowAlloc) free(rowAlloc);
        if (tables) free(tables);
        free(grayBuf);
        return false;
    }

    uint8_t *rawRow = rowAlloc;
    uint8_t *curRow = rowAlloc + stride;
    uint8_t *prevRow = curRow + rowBytes;
    memset(prevRow, 0, static_cast<size_t>(rowBytes));

    HuffTable &litLenTable = tables[0];
    HuffTable &distTable = tables[1];
    HuffTable &codeLenTable = tables[2];

    size_t winPos = 0;
    size_t inRowPos = 0;
    int curY = 0;

    auto flushScanline = [&]() {
        if (curY >= h) {
            inRowPos = 0;
            return;
        }
        const uint8_t *pixelRow = nullptr;
        if (hasPngTag) {
            uint8_t filterType = rawRow[0];
            const uint8_t *srcBytes = rawRow + 1;
            for (int c = 0; c < rowBytes; ++c) {
                uint8_t raw = srcBytes[c];
                uint8_t left = (c >= bpp) ? curRow[c - bpp] : 0;
                uint8_t up = (curY > 0) ? prevRow[c] : 0;
                uint8_t upLeft = (curY > 0 && c >= bpp) ? prevRow[c - bpp] : 0;
                switch (filterType) {
                    case 0: curRow[c] = raw; break;
                    case 1: curRow[c] = static_cast<uint8_t>(raw + left); break;
                    case 2: curRow[c] = static_cast<uint8_t>(raw + up); break;
                    case 3: curRow[c] = static_cast<uint8_t>(raw + ((static_cast<int>(left) + static_cast<int>(up)) >> 1)); break;
                    case 4: curRow[c] = static_cast<uint8_t>(raw + flatePaethPredictor(left, up, upLeft)); break;
                    default: curRow[c] = raw; break;
                }
            }
            pixelRow = curRow;
        } else if (predictor == 2) {
            memcpy(curRow, rawRow, static_cast<size_t>(rowBytes));
            for (int c = bpp; c < rowBytes; ++c) {
                curRow[c] = static_cast<uint8_t>(curRow[c] + curRow[c - bpp]);
            }
            pixelRow = curRow;
        } else {
            pixelRow = rawRow;
        }

        int dstY = curY >> scaleShift;
        if ((curY & ((1 << scaleShift) - 1)) == 0 && dstY < dstH) {
            uint8_t *dstRow = grayBuf + static_cast<size_t>(dstY) * dstW;
            for (int dx = 0; dx < dstW; ++dx) {
                int sx = std::min(w - 1, dx << scaleShift);
                uint8_t gVal = 255;
                if (effBpc == 1) {
                    int bit = (pixelRow[sx / 8] >> (7 - (sx % 8))) & 1;
                    if (invertBits) bit ^= 1;
                    if (!paletteGray.empty() && static_cast<size_t>(bit) < paletteGray.size()) {
                        gVal = paletteGray[bit];
                    } else {
                        gVal = bit ? 255 : 0;
                    }
                } else if (effBpc == 2 || effBpc == 4) {
                    int pixPerByte = 8 / effBpc;
                    int mask = (1 << effBpc) - 1;
                    int shift = (pixPerByte - 1 - (sx % pixPerByte)) * effBpc;
                    int val = (pixelRow[sx / pixPerByte] >> shift) & mask;
                    if (!paletteGray.empty() && static_cast<size_t>(val) < paletteGray.size()) {
                        gVal = paletteGray[val];
                    } else {
                        gVal = static_cast<uint8_t>((val * 255) / mask);
                    }
                } else if (effComps == 1 && effBpc == 8) {
                    uint8_t idx = pixelRow[sx];
                    if (!paletteGray.empty()) {
                        gVal = (idx < paletteGray.size()) ? paletteGray[idx] : idx;
                    } else {
                        gVal = invertBits ? (255 - idx) : idx;
                    }
                } else if (effComps == 3 && effBpc == 8) {
                    uint8_t r = pixelRow[sx * 3];
                    uint8_t g = pixelRow[sx * 3 + 1];
                    uint8_t b = pixelRow[sx * 3 + 2];
                    gVal = static_cast<uint8_t>((r * 77 + g * 150 + b * 29) >> 8);
                } else if (effComps == 4 && effBpc == 8) {
                    uint8_t c = pixelRow[sx * 4];
                    uint8_t m = pixelRow[sx * 4 + 1];
                    uint8_t y = pixelRow[sx * 4 + 2];
                    uint8_t k = pixelRow[sx * 4 + 3];
                    uint8_t r = 255 - std::min(255, (c * (255 - k)) / 255 + k);
                    uint8_t g = 255 - std::min(255, (m * (255 - k)) / 255 + k);
                    uint8_t b = 255 - std::min(255, (y * (255 - k)) / 255 + k);
                    gVal = static_cast<uint8_t>((r * 77 + g * 150 + b * 29) >> 8);
                }
                dstRow[dx] = gVal;
            }
        }

        if (hasPngTag) {
            std::swap(curRow, prevRow);
        }
        inRowPos = 0;
        curY++;
    };

    auto emitByte = [&](uint8_t b) {
        winBuf[winPos & kWinMask] = b;
        winPos++;
        rawRow[inRowPos++] = b;
        if (inRowPos == stride) {
            flushScanline();
        }
    };

    uint32_t bitBuf = 0;
    int bitCount = 0;

    auto ensureBits = [&](int n) {
        while (bitCount < n && bytePos < srcLen) {
            bitBuf |= static_cast<uint32_t>(src[bytePos++]) << bitCount;
            bitCount += 8;
        }
    };

    auto readBits = [&](int n) -> int {
        if (n == 0) return 0;
        ensureBits(n);
        if (bitCount < n) return -1;
        int val = static_cast<int>(bitBuf & ((1U << n) - 1U));
        bitBuf >>= n;
        bitCount -= n;
        return val;
    };

    auto decodeSym = [&](const HuffTable &ht) -> int {
        ensureBits(15);
        uint16_t f = ht.fast[bitBuf & ((1 << HuffTable::kFastBits) - 1)];
        if (f != 0) {
            int len = f >> 9;
            if (len > bitCount) return -1;
            bitBuf >>= len;
            bitCount -= len;
            return f & 0x1FF;
        }
        int32_t code = 0;
        for (int bits = 1; bits <= HuffTable::kMaxBits; ++bits) {
            if (bits > bitCount) return -1;
            code = (code << 1) | static_cast<int32_t>((bitBuf >> (bits - 1)) & 1);
            int32_t first = ht.firstCode[bits];
            int32_t off = code - first;
            if (off >= 0 && off < ht.count[bits]) {
                bitBuf >>= bits;
                bitCount -= bits;
                return ht.syms[ht.firstIdx[bits] + off];
            }
        }
        return -1;
    };

    static const uint16_t kLenBase[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
    };
    static const uint8_t kLenExtra[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
    };
    static const uint16_t kDistBase[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
    };
    static const uint8_t kDistExtra[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
    };
    static const uint8_t kCodeLenOrder[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };

    bool ok = true;
    int bfinal = 0;
    while (!bfinal && ok && curY < h) {
        bfinal = readBits(1);
        int btype = readBits(2);
        if (bfinal < 0 || btype < 0 || btype == 3) {
            ok = false;
            break;
        }

        if (btype == 0) {
            int drop = bitCount & 7;
            bitBuf >>= drop;
            bitCount -= drop;
            ensureBits(32);
            if (bitCount < 32) { ok = false; break; }
            uint16_t bLen = static_cast<uint16_t>(bitBuf & 0xFFFF);
            uint16_t bNlen = static_cast<uint16_t>((bitBuf >> 16) & 0xFFFF);
            bitBuf = 0;
            bitCount = 0;
            if (bLen != static_cast<uint16_t>(~bNlen) || bytePos + bLen > srcLen) {
                ok = false;
                break;
            }
            for (uint16_t i = 0; i < bLen; ++i) {
                emitByte(src[bytePos++]);
            }
            continue;
        }

        if (btype == 1) {
            uint8_t fixedLit[288];
            for (int i = 0; i <= 143; ++i) fixedLit[i] = 8;
            for (int i = 144; i <= 255; ++i) fixedLit[i] = 9;
            for (int i = 256; i <= 279; ++i) fixedLit[i] = 7;
            for (int i = 280; i <= 287; ++i) fixedLit[i] = 8;
            uint8_t fixedDist[32];
            for (int i = 0; i < 32; ++i) fixedDist[i] = 5;
            if (!litLenTable.build(fixedLit, 288) || !distTable.build(fixedDist, 32)) {
                ok = false;
                break;
            }
        } else {
            int hlit = readBits(5);
            int hdist = readBits(5);
            int hclen = readBits(4);
            if (hlit < 0 || hdist < 0 || hclen < 0) { ok = false; break; }
            hlit += 257;
            hdist += 1;
            hclen += 4;
            if (hlit > 286 || hdist > 30) { ok = false; break; }

            uint8_t clLengths[19];
            memset(clLengths, 0, sizeof(clLengths));
            for (int i = 0; i < hclen; ++i) {
                int v = readBits(3);
                if (v < 0) { ok = false; break; }
                clLengths[kCodeLenOrder[i]] = static_cast<uint8_t>(v);
            }
            if (!ok || !codeLenTable.build(clLengths, 19)) { ok = false; break; }

            uint8_t lengths[320];
            memset(lengths, 0, sizeof(lengths));
            int totalSyms = hlit + hdist;
            int idx = 0;
            while (idx < totalSyms) {
                int sym = decodeSym(codeLenTable);
                if (sym < 0) { ok = false; break; }
                if (sym <= 15) {
                    lengths[idx++] = static_cast<uint8_t>(sym);
                } else if (sym == 16) {
                    int rep = readBits(2);
                    if (rep < 0 || idx == 0) { ok = false; break; }
                    rep += 3;
                    uint8_t prev = lengths[idx - 1];
                    while (rep-- > 0 && idx < totalSyms) lengths[idx++] = prev;
                } else if (sym == 17) {
                    int rep = readBits(3);
                    if (rep < 0) { ok = false; break; }
                    rep += 3;
                    while (rep-- > 0 && idx < totalSyms) lengths[idx++] = 0;
                } else if (sym == 18) {
                    int rep = readBits(7);
                    if (rep < 0) { ok = false; break; }
                    rep += 11;
                    while (rep-- > 0 && idx < totalSyms) lengths[idx++] = 0;
                } else {
                    ok = false;
                    break;
                }
            }
            if (!ok ||
                !litLenTable.build(lengths, hlit) ||
                !distTable.build(lengths + hlit, hdist))
            {
                ok = false;
                break;
            }
        }

        while (curY < h) {
            int sym = decodeSym(litLenTable);
            if (sym < 0) {
                if (bytePos >= srcLen && winPos > 0) {
                    bfinal = 1;
                } else {
                    ok = false;
                }
                break;
            }
            if (sym < 256) {
                emitByte(static_cast<uint8_t>(sym));
            } else if (sym == 256) {
                break;
            } else if (sym <= 285) {
                int lenIdx = sym - 257;
                int extraL = readBits(kLenExtra[lenIdx]);
                if (extraL < 0) { ok = false; break; }
                size_t matchLen = static_cast<size_t>(kLenBase[lenIdx] + extraL);

                int distSym = decodeSym(distTable);
                if (distSym < 0 || distSym >= 30) { ok = false; break; }
                int extraD = readBits(kDistExtra[distSym]);
                if (extraD < 0) { ok = false; break; }
                size_t matchDist = static_cast<size_t>(kDistBase[distSym] + extraD);

                if (matchDist == 0 || matchDist > winPos || matchDist > kWinSize) {
                    ok = false;
                    break;
                }
                for (size_t i = 0; i < matchLen; ++i) {
                    uint8_t b = winBuf[(winPos - matchDist) & kWinMask];
                    emitByte(b);
                }
            } else {
                ok = false;
                break;
            }
        }
    }

    // Check if an unannounced PNG predictor was actually used (or vice versa) when winPos > 0
    size_t exactPngBytes = ( static_cast<size_t>(rowBytes) + 1 ) * static_cast<size_t>(h);
    size_t exactRawBytes = static_cast<size_t>(rowBytes) * static_cast<size_t>(h);
    bool mismatchRetry = false;
    int retryPredictor = predictor;
    if (winPos == exactPngBytes && !hasPngTag) {
        mismatchRetry = true;
        retryPredictor = 12;
    } else if (winPos == exactRawBytes && hasPngTag) {
        mismatchRetry = true;
        retryPredictor = 1;
    }

    free(winBuf);
    free(rowAlloc);
    free(tables);

    if (mismatchRetry) {
        free(grayBuf);
        return tryStreamingFlateImageToGray(
            src, srcLen, windowBits, w, h, bpc, comps, retryPredictor, predictorCols,
            invertBits, paletteGray, outGrayBuf, outW, outH);
    }

    if ((ok || curY > h / 2) && curY > 0) {
        outGrayBuf = grayBuf;
        outW = dstW;
        outH = dstH;
        return true;
    }

    free(grayBuf);
    return false;
}

bool inflatePdfFlateImageToGray(
    const uint8_t *src,
    size_t srcLen,
    int w,
    int h,
    int bpc,
    int comps,
    int predictor,
    int predictorCols,
    bool invertBits,
    const std::vector<uint8_t> &paletteGray,
    uint8_t *&outGrayBuf,
    int &outW,
    int &outH)
{
    outGrayBuf = nullptr;
    outW = outH = 0;
    if (!src || srcLen == 0 || w <= 0 || h <= 0) return false;

    size_t trimmedLen = srcLen;
    while (trimmedLen > 2 && (src[trimmedLen - 1] == '\n' || src[trimmedLen - 1] == '\r' || src[trimmedLen - 1] == ' ')) {
        trimmedLen--;
    }

    if (tryStreamingFlateImageToGray(src, srcLen, 15, w, h, bpc, comps, predictor, predictorCols, invertBits, paletteGray, outGrayBuf, outW, outH)) {
        return true;
    }
    if (trimmedLen != srcLen && tryStreamingFlateImageToGray(src, trimmedLen, 15, w, h, bpc, comps, predictor, predictorCols, invertBits, paletteGray, outGrayBuf, outW, outH)) {
        return true;
    }
    if (tryStreamingFlateImageToGray(src, srcLen, -15, w, h, bpc, comps, predictor, predictorCols, invertBits, paletteGray, outGrayBuf, outW, outH)) {
        return true;
    }
    if (trimmedLen > 2 && tryStreamingFlateImageToGray(src + 2, trimmedLen - 2, -15, w, h, bpc, comps, predictor, predictorCols, invertBits, paletteGray, outGrayBuf, outW, outH)) {
        return true;
    }
    return false;
}

// =============================================================================
// 1b. JBIG2 Bi-Level Image Decoder (ITU-T T.88 Annex E MQ + Generic Region)
// =============================================================================
namespace {

struct Jbig2MqState {
    uint16_t qe;
    uint8_t nmps;
    uint8_t nlps;
    uint8_t sw;
};

static const Jbig2MqState kJbig2MqTable[47] = {
    {0x5601,  1,  1, 1}, {0x3401,  2,  6, 0}, {0x1801,  3,  9, 0}, {0x0AC1,  4, 12, 0},
    {0x0521,  5, 29, 0}, {0x0221, 38, 33, 0}, {0x5601,  7,  6, 1}, {0x5401,  8, 14, 0},
    {0x4801,  9, 14, 0}, {0x3801, 10, 14, 0}, {0x3001, 11, 17, 0}, {0x2401, 12, 18, 0},
    {0x1C01, 13, 20, 0}, {0x1601, 29, 21, 0}, {0x5601, 15, 14, 1}, {0x5401, 16, 14, 0},
    {0x5101, 17, 15, 0}, {0x4801, 18, 16, 0}, {0x3801, 19, 17, 0}, {0x3401, 20, 18, 0},
    {0x3001, 21, 19, 0}, {0x2801, 22, 19, 0}, {0x2401, 23, 20, 0}, {0x2201, 24, 21, 0},
    {0x1C01, 25, 22, 0}, {0x1801, 26, 23, 0}, {0x1601, 27, 24, 0}, {0x1401, 28, 25, 0},
    {0x1201, 29, 26, 0}, {0x1101, 30, 27, 0}, {0x0AC1, 31, 28, 0}, {0x09C1, 32, 29, 0},
    {0x08A1, 33, 30, 0}, {0x0521, 34, 31, 0}, {0x0441, 35, 32, 0}, {0x02A1, 36, 33, 0},
    {0x0221, 37, 34, 0}, {0x0141, 38, 35, 0}, {0x0111, 39, 36, 0}, {0x0085, 40, 37, 0},
    {0x0049, 41, 38, 0}, {0x0025, 42, 39, 0}, {0x0015, 43, 40, 0}, {0x0009, 44, 41, 0},
    {0x0005, 45, 42, 0}, {0x0001, 45, 43, 0}, {0x5601, 46, 46, 0}
};

struct Jbig2MqDecoder {
    const uint8_t *data = nullptr;
    size_t len = 0;
    size_t bp = 0;
    uint32_t a = 0x8000;
    uint32_t c = 0;
    int ct = 0;

    void byteIn() {
        uint8_t b = (bp < len) ? data[bp] : 0xFF;
        if (b == 0xFF) {
            uint8_t b1 = (bp + 1 < len) ? data[bp + 1] : 0xFF;
            if (b1 > 0x8F) {
                c += 0xFF00U;
                ct = 8;
            } else {
                bp++;
                c += static_cast<uint32_t>(b1) << 9;
                ct = 7;
            }
        } else {
            bp++;
            uint8_t nextB = (bp < len) ? data[bp] : 0xFF;
            c += static_cast<uint32_t>(nextB) << 8;
            ct = 8;
        }
    }

    void init(const uint8_t *src, size_t srcLen) {
        data = src;
        len = srcLen;
        bp = 0;
        uint8_t b0 = (len > 0) ? data[0] : 0xFF;
        c = static_cast<uint32_t>(b0) << 16;
        byteIn();
        c <<= 7;
        ct -= 7;
        a = 0x8000U;
    }

    inline int decodeBit(uint8_t &ctx) {
        uint8_t idx = ctx & 0x7F;
        uint8_t mps = (ctx >> 7) & 1;
        uint32_t qe = kJbig2MqTable[idx].qe;
        a -= qe;
        int d;
        if ((c >> 16) < qe) {
            if (a < qe) {
                a = qe;
                d = mps;
                ctx = static_cast<uint8_t>((mps << 7) | kJbig2MqTable[idx].nmps);
            } else {
                a = qe;
                d = 1 - mps;
                if (kJbig2MqTable[idx].sw) mps ^= 1;
                ctx = static_cast<uint8_t>((mps << 7) | kJbig2MqTable[idx].nlps);
            }
            do {
                if (ct == 0) byteIn();
                a <<= 1;
                c <<= 1;
                ct--;
            } while ((a & 0x8000U) == 0);
        } else {
            c -= (qe << 16);
            if ((a & 0x8000U) == 0) {
                if (a < qe) {
                    d = 1 - mps;
                    if (kJbig2MqTable[idx].sw) mps ^= 1;
                    ctx = static_cast<uint8_t>((mps << 7) | kJbig2MqTable[idx].nlps);
                } else {
                    d = mps;
                    ctx = static_cast<uint8_t>((mps << 7) | kJbig2MqTable[idx].nmps);
                }
                do {
                    if (ct == 0) byteIn();
                    a <<= 1;
                    c <<= 1;
                    ct--;
                } while ((a & 0x8000U) == 0);
            } else {
                d = mps;
            }
        }
        return d;
    }
};

} // namespace

bool decodePdfJbig2ToGray(
    const uint8_t *src,
    size_t srcLen,
    int fallbackW,
    int fallbackH,
    bool invertBits,
    uint8_t *&outGrayBuf,
    int &outW,
    int &outH)
{
    outGrayBuf = nullptr;
    outW = outH = 0;
    if (!src || srcLen < 11) return false;

    size_t p = 0;
    // Skip optional 8-byte standalone JBIG2 file header if present
    static const uint8_t kJbig2Magic[8] = { 0x97, 0x4A, 0x42, 0x32, 0x0D, 0x0A, 0x1A, 0x0A };
    if (srcLen >= 16 && memcmp(src, kJbig2Magic, 8) == 0) {
        p = 8;
        uint8_t fileFlags = src[p++];
        if ((fileFlags & 2) == 0 && p + 4 <= srcLen) {
            p += 4; // number of pages
        }
    }

    int pageW = fallbackW;
    int pageH = fallbackH;
    bool decodedAnyRegion = false;

    // Parse JBIG2 segments
    while (p + 11 <= srcLen) {
        uint32_t segNum = readU32BE(src + p);
        uint8_t segFlags = src[p + 4];
        int segType = segFlags & 0x3F;
        bool pageAssocLarge = (segFlags & 0x40) != 0;
        p += 5;

        if (p >= srcLen) break;
        uint8_t rtByte = src[p];
        int rtCount = (rtByte >> 5) & 7;
        size_t refSize = (segNum <= 256) ? 1 : ((segNum <= 65536) ? 2 : 4);
        if (rtCount < 7) {
            p += 1 + static_cast<size_t>(rtCount) * refSize;
        } else {
            if (p + 4 > srcLen) break;
            uint32_t longCnt = readU32BE(src + p) & 0x1FFFFFFFU;
            p += 4 + ((static_cast<size_t>(longCnt) + 8) / 8) + static_cast<size_t>(longCnt) * refSize;
        }

        p += pageAssocLarge ? 4 : 1;
        if (p + 4 > srcLen) break;
        uint32_t dataLen = readU32BE(src + p);
        p += 4;

        size_t availLen = (dataLen == 0xFFFFFFFFU || p + dataLen > srcLen) ? (srcLen - p) : static_cast<size_t>(dataLen);
        const uint8_t *segData = src + p;

        if (segType == 48 && availLen >= 19) {
            // Segment Type 48: Page Information
            int pw = static_cast<int>(readU32BE(segData));
            int ph = static_cast<int>(readU32BE(segData + 4));
            if (pw > 0 && pw <= 16384) pageW = pw;
            if (ph > 0 && ph <= 16384) pageH = ph;
        } else if ((segType == 36 || segType == 38 || segType == 39) && availLen >= 18) {
            // Segment Type 36/38/39: Generic Region
            int regW = static_cast<int>(readU32BE(segData));
            int regH = static_cast<int>(readU32BE(segData + 4));
            uint8_t gFlags = segData[17];
            bool mmr = (gFlags & 1) != 0;
            int gbTemplate = (gFlags >> 1) & 3;
            bool tpgdon = ((gFlags >> 3) & 1) != 0;
            bool extTemplate = ((gFlags >> 4) & 1) != 0;

            size_t hdrOff = 18;
            int8_t atx[4] = { 3, -3, 2, -2 };
            int8_t aty[4] = { -1, -1, -2, -2 };
            if (!mmr) {
                int numAt = (gbTemplate == 0) ? (extTemplate ? 12 : 4) : 1;
                if (hdrOff + static_cast<size_t>(numAt) * 2 > availLen) break;
                for (int i = 0; i < std::min(4, numAt); ++i) {
                    atx[i] = static_cast<int8_t>(segData[hdrOff + i * 2]);
                    aty[i] = static_cast<int8_t>(segData[hdrOff + i * 2 + 1]);
                }
                hdrOff += static_cast<size_t>(numAt) * 2;
            }

            if (!mmr && regW > 0 && regH > 0 && regW <= 16384 && regH <= 16384 && hdrOff < availLen) {
                int scaleShift = 0;
                while (((regW >> scaleShift) > 1296 || (regH >> scaleShift) > 1296) && scaleShift < 3) {
                    scaleShift++;
                }
                int dstW = std::max(1, regW >> scaleShift);
                int dstH = std::max(1, regH >> scaleShift);

                uint8_t *grayBuf = allocComicPageBuffer(static_cast<size_t>(dstW) * dstH);
                uint8_t *ctxTable = allocComicPageBuffer(65536);
                // 8-row ring buffer of 1-byte-per-pixel bi-level values (0 or 1) with 8-pixel left/right padding
                constexpr int kRingRows = 8;
                constexpr int kPad = 8;
                int rowStride = regW + kPad * 2;
                uint8_t *ringBuf = allocComicPageBuffer(static_cast<size_t>(kRingRows) * rowStride);
                if (!grayBuf || !ctxTable || !ringBuf) {
                    if (grayBuf) free(grayBuf);
                    if (ctxTable) free(ctxTable);
                    if (ringBuf) free(ringBuf);
                    return false;
                }

                memset(grayBuf, 255, static_cast<size_t>(dstW) * dstH);
                memset(ctxTable, 0, 65536);
                memset(ringBuf, 0, static_cast<size_t>(kRingRows) * rowStride);

                Jbig2MqDecoder mq;
                mq.init(segData + hdrOff, availLen - hdrOff);

                const bool defaultAt0 = (gbTemplate == 0 && !extTemplate &&
                                         atx[0] == 3 && aty[0] == -1 &&
                                         atx[1] == -3 && aty[1] == -1 &&
                                         atx[2] == 2 && aty[2] == -2 &&
                                         atx[3] == -2 && aty[3] == -2);

                static const uint16_t kTpgdCtx[4] = { 0x9B25, 0x0795, 0x00E5, 0x0195 };
                int ltp = 0;

                auto getRingPx = [&](int rx, int ry) -> uint32_t {
                    if (ry < 0 || rx < 0 || rx >= regW) return 0U;
                    int rIdx = ry & (kRingRows - 1);
                    return ringBuf[rIdx * rowStride + kPad + rx];
                };

                for (int y = 0; y < regH; ++y) {
                    uint8_t *curRow = ringBuf + (y & (kRingRows - 1)) * rowStride + kPad;
                    const uint8_t *rowM1 = (y >= 1) ? (ringBuf + ((y - 1) & (kRingRows - 1)) * rowStride + kPad) : (ringBuf + ((kRingRows - 1) * rowStride + kPad));
                    const uint8_t *rowM2 = (y >= 2) ? (ringBuf + ((y - 2) & (kRingRows - 1)) * rowStride + kPad) : (ringBuf + ((kRingRows - 1) * rowStride + kPad));

                    if (tpgdon) {
                        ltp ^= mq.decodeBit(ctxTable[kTpgdCtx[gbTemplate]]);
                    }

                    if (ltp) {
                        if (y >= 1) {
                            memcpy(curRow, rowM1, static_cast<size_t>(regW));
                        } else {
                            memset(curRow, 0, static_cast<size_t>(regW));
                        }
                    } else if (defaultAt0) {
                        // Fast sliding-window path for standard Template 0 (used on all 124 pages of adults.pdf!)
                        uint32_t c2 = (y >= 2) ? ((rowM2[0] << 2) | (rowM2[1] << 1) | rowM2[2]) : 0U;
                        uint32_t c1 = (y >= 1) ? ((rowM1[0] << 3) | (rowM1[1] << 2) | (rowM1[2] << 1) | rowM1[3]) : 0U;
                        uint32_t c0 = 0U;
                        for (int x = 0; x < regW; ++x) {
                            uint32_t ctx = (c2 << 11) | (c1 << 4) | c0;
                            uint8_t d = static_cast<uint8_t>(mq.decodeBit(ctxTable[ctx]));
                            curRow[x] = d;
                            c2 = ((c2 << 1) | ((y >= 2) ? rowM2[x + 3] : 0U)) & 0x1FU;
                            c1 = ((c1 << 1) | ((y >= 1) ? rowM1[x + 4] : 0U)) & 0x7FU;
                            c0 = ((c0 << 1) | d) & 0x0FU;
                        }
                    } else if (gbTemplate == 0) {
                        for (int x = 0; x < regW; ++x) {
                            uint32_t ctx =
                                (getRingPx(x + atx[3], y + aty[3]) << 15) |
                                (getRingPx(x - 1, y - 2) << 14) |
                                (getRingPx(x,     y - 2) << 13) |
                                (getRingPx(x + 1, y - 2) << 12) |
                                (getRingPx(x + atx[2], y + aty[2]) << 11) |
                                (getRingPx(x + atx[1], y + aty[1]) << 10) |
                                (getRingPx(x - 2, y - 1) << 9) |
                                (getRingPx(x - 1, y - 1) << 8) |
                                (getRingPx(x,     y - 1) << 7) |
                                (getRingPx(x + 1, y - 1) << 6) |
                                (getRingPx(x + 2, y - 1) << 5) |
                                (getRingPx(x + atx[0], y + aty[0]) << 4) |
                                (getRingPx(x - 4, y) << 3) |
                                (getRingPx(x - 3, y) << 2) |
                                (getRingPx(x - 2, y) << 1) |
                                getRingPx(x - 1, y);
                            curRow[x] = static_cast<uint8_t>(mq.decodeBit(ctxTable[ctx]));
                        }
                    } else if (gbTemplate == 1) {
                        for (int x = 0; x < regW; ++x) {
                            uint32_t ctx =
                                (getRingPx(x - 1, y - 2) << 12) |
                                (getRingPx(x,     y - 2) << 11) |
                                (getRingPx(x + 1, y - 2) << 10) |
                                (getRingPx(x + 2, y - 2) << 9) |
                                (getRingPx(x - 2, y - 1) << 8) |
                                (getRingPx(x - 1, y - 1) << 7) |
                                (getRingPx(x,     y - 1) << 6) |
                                (getRingPx(x + 1, y - 1) << 5) |
                                (getRingPx(x + 2, y - 1) << 4) |
                                (getRingPx(x + atx[0], y + aty[0]) << 3) |
                                (getRingPx(x - 3, y) << 2) |
                                (getRingPx(x - 2, y) << 1) |
                                getRingPx(x - 1, y);
                            curRow[x] = static_cast<uint8_t>(mq.decodeBit(ctxTable[ctx]));
                        }
                    } else if (gbTemplate == 2) {
                        for (int x = 0; x < regW; ++x) {
                            uint32_t ctx =
                                (getRingPx(x - 1, y - 2) << 9) |
                                (getRingPx(x,     y - 2) << 8) |
                                (getRingPx(x + 1, y - 2) << 7) |
                                (getRingPx(x - 2, y - 1) << 6) |
                                (getRingPx(x - 1, y - 1) << 5) |
                                (getRingPx(x,     y - 1) << 4) |
                                (getRingPx(x + 1, y - 1) << 3) |
                                (getRingPx(x + atx[0], y + aty[0]) << 2) |
                                (getRingPx(x - 2, y) << 1) |
                                getRingPx(x - 1, y);
                            curRow[x] = static_cast<uint8_t>(mq.decodeBit(ctxTable[ctx]));
                        }
                    } else {
                        for (int x = 0; x < regW; ++x) {
                            uint32_t ctx =
                                (getRingPx(x - 3, y - 1) << 9) |
                                (getRingPx(x - 2, y - 1) << 8) |
                                (getRingPx(x - 1, y - 1) << 7) |
                                (getRingPx(x,     y - 1) << 6) |
                                (getRingPx(x + 1, y - 1) << 5) |
                                (getRingPx(x + atx[0], y + aty[0]) << 4) |
                                (getRingPx(x - 4, y) << 3) |
                                (getRingPx(x - 3, y) << 2) |
                                (getRingPx(x - 2, y) << 1) |
                                getRingPx(x - 1, y);
                            curRow[x] = static_cast<uint8_t>(mq.decodeBit(ctxTable[ctx]));
                        }
                    }

                    // Box-downsample completed block of (1 << scaleShift) rows into grayBuf, preserving crisp ink
                    int blockMask = (1 << scaleShift) - 1;
                    if ((y & blockMask) == blockMask || y == regH - 1) {
                        int dstY = y >> scaleShift;
                        if (dstY < dstH) {
                            uint8_t *dstRow = grayBuf + static_cast<size_t>(dstY) * dstW;
                            int yStart = dstY << scaleShift;
                            int yEnd = std::min(regH, yStart + (1 << scaleShift));
                            int step = 1 << scaleShift;
                            for (int dx = 0; dx < dstW; ++dx) {
                                int xStart = dx << scaleShift;
                                int xEnd = std::min(regW, xStart + step);
                                int inkCount = 0;
                                int totalCount = 0;
                                for (int sy = yStart; sy < yEnd; ++sy) {
                                    const uint8_t *sRow = ringBuf + (sy & (kRingRows - 1)) * rowStride + kPad;
                                    for (int sx = xStart; sx < xEnd; ++sx) {
                                        int bit = sRow[sx] & 1;
                                        if (invertBits) bit ^= 1;
                                        inkCount += bit;
                                        totalCount++;
                                    }
                                }
                                // In JBIG2, bit=1 is black ink (0) and bit=0 is white paper (255)
                                int gray = 255 - (inkCount * 255) / std::max(1, totalCount);
                                dstRow[dx] = static_cast<uint8_t>(gray);
                            }
                        }
                    }
                }

                free(ctxTable);
                free(ringBuf);
                outGrayBuf = grayBuf;
                outW = dstW;
                outH = dstH;
                decodedAnyRegion = true;
                break;
            }
        }

        if (dataLen == 0xFFFFFFFFU || p + availLen >= srcLen) break;
        p += availLen;
    }

    (void)pageW;
    (void)pageH;
    return decodedAnyRegion && outGrayBuf != nullptr;
}

bool decodePdfAsciiHex(const uint8_t *src, size_t len, std::vector<uint8_t> &out) {
    out.clear();
    if (!src || len == 0) return false;
    out.reserve(len / 2);

    int hi = -1;
    for (size_t i = 0; i < len; ++i) {
        char c = static_cast<char>(src[i]);
        if (c == '>') break; // EOD marker
        int v = hexDigitVal(c);
        if (v < 0) continue;
        if (hi < 0) {
            hi = v;
        } else {
            out.push_back(static_cast<uint8_t>((hi << 4) | v));
            hi = -1;
        }
    }
    if (hi >= 0) {
        out.push_back(static_cast<uint8_t>(hi << 4));
    }
    return true;
}

bool decodePdfAscii85(const uint8_t *src, size_t len, std::vector<uint8_t> &out) {
    out.clear();
    if (!src || len == 0) return false;
    out.reserve((len * 4) / 5);

    size_t i = 0;
    if (len >= 2 && src[0] == '<' && src[1] == '~') {
        i = 2;
    }

    uint32_t tuple = 0;
    int count = 0;

    for (; i < len; ++i) {
        uint8_t c = src[i];
        if (c == '~' && i + 1 < len && src[i + 1] == '>') {
            break; // EOD
        }
        if (c <= ' ') continue; // whitespace
        if (c == 'z') {
            if (count != 0) return false;
            out.push_back(0);
            out.push_back(0);
            out.push_back(0);
            out.push_back(0);
            continue;
        }
        if (c < '!' || c > 'u') continue;

        tuple = tuple * 85 + static_cast<uint32_t>(c - '!');
        if (++count == 5) {
            out.push_back(static_cast<uint8_t>((tuple >> 24) & 0xFF));
            out.push_back(static_cast<uint8_t>((tuple >> 16) & 0xFF));
            out.push_back(static_cast<uint8_t>((tuple >> 8) & 0xFF));
            out.push_back(static_cast<uint8_t>(tuple & 0xFF));
            tuple = 0;
            count = 0;
        }
    }

    if (count > 1) {
        for (int pad = count; pad < 5; ++pad) {
            tuple = tuple * 85 + 84;
        }
        for (int b = 0; b < count - 1; ++b) {
            out.push_back(static_cast<uint8_t>((tuple >> (24 - b * 8)) & 0xFF));
        }
    }
    return true;
}

bool decodePdfRunLength(const uint8_t *src, size_t len, std::vector<uint8_t> &out) {
    out.clear();
    if (!src || len == 0) return false;
    out.reserve(len);

    size_t i = 0;
    while (i < len) {
        uint8_t b = src[i++];
        if (b == 128) {
            break; // EOD
        } else if (b < 128) {
            size_t copyLen = static_cast<size_t>(b) + 1;
            size_t avail = std::min(copyLen, len - i);
            out.insert(out.end(), src + i, src + i + avail);
            i += avail;
        } else {
            if (i >= len) break;
            size_t repeatCount = 257 - static_cast<size_t>(b);
            uint8_t val = src[i++];
            out.insert(out.end(), repeatCount, val);
        }
    }
    return true;
}

bool decodePdfLzw(const uint8_t *src, size_t len, int earlyChange, std::vector<uint8_t> &out) {
    out.clear();
    if (!src || len == 0) return false;

    struct LzwEntry {
        int16_t prefix;
        uint8_t byte;
        uint16_t length;
    };
    std::vector<LzwEntry> table(4096);
    auto resetTable = [&]() {
        for (int i = 0; i < 256; ++i) {
            table[i] = { -1, static_cast<uint8_t>(i), 1 };
        }
    };
    resetTable();

    int nextCode = 258;
    int codeBits = 9;
    int prevCode = -1;
    size_t bitPos = 0;
    const size_t totalBits = len * 8;
    const int ec = (earlyChange != 0) ? 1 : 0;

    auto readBits = [&](int nBits) -> int {
        if (bitPos + static_cast<size_t>(nBits) > totalBits) return 257;
        int val = 0;
        for (int i = 0; i < nBits; ++i) {
            size_t bIdx = (bitPos + i) >> 3;
            int bitShift = 7 - static_cast<int>((bitPos + i) & 7);
            val = (val << 1) | ((src[bIdx] >> bitShift) & 1);
        }
        bitPos += static_cast<size_t>(nBits);
        return val;
    };

    std::vector<uint8_t> seqBuf;
    seqBuf.reserve(4096);

    auto decodeCodeToBuf = [&](int code, std::vector<uint8_t> &dst) -> bool {
        dst.clear();
        int cur = code;
        int safety = 0;
        while (cur >= 0 && cur < nextCode && safety++ < 4096) {
            dst.push_back(table[cur].byte);
            cur = table[cur].prefix;
        }
        std::reverse(dst.begin(), dst.end());
        return !dst.empty();
    };

    std::vector<uint8_t> prevSeq;
    while (bitPos < totalBits && out.size() < 16 * 1024 * 1024) {
        int code = readBits(codeBits);
        if (code == 257) break; // EOD
        if (code == 256) {      // Clear table
            resetTable();
            nextCode = 258;
            codeBits = 9;
            prevCode = -1;
            prevSeq.clear();
            continue;
        }

        if (code < nextCode) {
            if (!decodeCodeToBuf(code, seqBuf)) break;
        } else if (code == nextCode && prevCode >= 0 && !prevSeq.empty()) {
            seqBuf = prevSeq;
            seqBuf.push_back(prevSeq[0]);
        } else {
            break;
        }

        out.insert(out.end(), seqBuf.begin(), seqBuf.end());

        if (prevCode >= 0 && nextCode < 4096) {
            table[nextCode] = {
                static_cast<int16_t>(prevCode),
                seqBuf[0],
                static_cast<uint16_t>(prevSeq.size() + 1)
            };
            nextCode++;
            if (nextCode == (512 - ec) && codeBits == 9) codeBits = 10;
            else if (nextCode == (1024 - ec) && codeBits == 10) codeBits = 11;
            else if (nextCode == (2048 - ec) && codeBits == 11) codeBits = 12;
        }

        prevCode = code;
        prevSeq = seqBuf;
    }

    return !out.empty();
}

// =============================================================================
// 2. PDF Character Encodings & Standard Adobe Glyph Names
// =============================================================================

uint32_t pdfWinAnsiToUnicode(uint8_t code) {
    if (code < 0x80 || code >= 0xA0) return code;
    static const uint16_t kWin1252Map[32] = {
        0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
        0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
        0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
    };
    return kWin1252Map[code - 0x80];
}

uint32_t pdfMacRomanToUnicode(uint8_t code) {
    if (code < 0x80) return code;
    static const uint16_t kMacRomanMap[128] = {
        0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1,
        0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
        0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3,
        0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
        0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF,
        0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
        0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211,
        0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8,
        0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB,
        0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
        0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA,
        0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
        0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1,
        0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
        0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC,
        0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7
    };
    return kMacRomanMap[code - 0x80];
}

uint32_t pdfGlyphNameToUnicode(const std::string &glyphName) {
    if (glyphName.empty()) return 0;
    std::string name = (glyphName[0] == '/') ? glyphName.substr(1) : glyphName;
    if (name.empty()) return 0;

    // Single ASCII character glyph name: /a .. /z, /A .. /Z, /0 .. /9
    if (name.size() == 1) {
        return static_cast<unsigned char>(name[0]);
    }

    // /uniXXXX or /uXXXX
    if (name.size() >= 5 && name.compare(0, 3, "uni") == 0 && std::isxdigit(static_cast<unsigned char>(name[3]))) {
        return static_cast<uint32_t>(strtoul(name.substr(3, 4).c_str(), nullptr, 16));
    }
    if (name.size() >= 5 && name[0] == 'u' && std::isxdigit(static_cast<unsigned char>(name[1])) &&
        std::isxdigit(static_cast<unsigned char>(name[2])) &&
        std::isxdigit(static_cast<unsigned char>(name[3])) &&
        std::isxdigit(static_cast<unsigned char>(name[4])))
    {
        return static_cast<uint32_t>(strtoul(name.substr(1).c_str(), nullptr, 16));
    }

    struct NameEntry {
        const char *name;
        uint16_t cp;
    };
    static const NameEntry kGlyphMap[] = {
        {"space", 0x0020}, {"exclam", 0x0021}, {"quotedbl", 0x0022}, {"numbersign", 0x0023},
        {"dollar", 0x0024}, {"percent", 0x0025}, {"ampersand", 0x0026}, {"quotesingle", 0x0027},
        {"parenleft", 0x0028}, {"parenright", 0x0029}, {"asterisk", 0x002A}, {"plus", 0x002B},
        {"comma", 0x002C}, {"hyphen", 0x002D}, {"minus", 0x2212}, {"period", 0x002E},
        {"slash", 0x002F}, {"zero", '0'}, {"one", '1'}, {"two", '2'}, {"three", '3'},
        {"four", '4'}, {"five", '5'}, {"six", '6'}, {"seven", '7'}, {"eight", '8'}, {"nine", '9'},
        {"colon", 0x003A}, {"semicolon", 0x003B}, {"less", 0x003C}, {"equal", 0x003D},
        {"greater", 0x003E}, {"question", 0x003F}, {"at", 0x0040}, {"bracketleft", 0x005B},
        {"backslash", 0x005C}, {"bracketright", 0x005D}, {"asciicircum", 0x005E},
        {"underscore", 0x005F}, {"grave", 0x0060}, {"braceleft", 0x007B}, {"bar", 0x007C},
        {"braceright", 0x007D}, {"asciitilde", 0x007E},
        {"nbspace", 0x00A0}, {"exclamdown", 0x00A1}, {"cent", 0x00A2}, {"sterling", 0x00A3},
        {"currency", 0x00A4}, {"yen", 0x00A5}, {"brokenbar", 0x00A6}, {"section", 0x00A7},
        {"dieresis", 0x00A8}, {"copyright", 0x00A9}, {"ordfeminine", 0x00AA}, {"guillemotleft", 0x00AB},
        {"logicalnot", 0x00AC}, {"registered", 0x00AE}, {"macron", 0x00AF}, {"degree", 0x00B0},
        {"plusminus", 0x00B1}, {"twosuperior", 0x00B2}, {"threesuperior", 0x00B3}, {"acute", 0x00B4},
        {"mu", 0x00B5}, {"paragraph", 0x00B6}, {"periodcentered", 0x00B7}, {"cedilla", 0x00B8},
        {"onesuperior", 0x00B9}, {"ordmasculine", 0x00BA}, {"guillemotright", 0x00BB},
        {"onequarter", 0x00BC}, {"onehalf", 0x00BD}, {"threequarters", 0x00BE}, {"questiondown", 0x00BF},
        {"Agrave", 0x00C0}, {"Aacute", 0x00C1}, {"Acircumflex", 0x00C2}, {"Atilde", 0x00C3},
        {"Adieresis", 0x00C4}, {"Aring", 0x00C5}, {"AE", 0x00C6}, {"Ccedilla", 0x00C7},
        {"Egrave", 0x00C8}, {"Eacute", 0x00C9}, {"Ecircumflex", 0x00CA}, {"Edieresis", 0x00CB},
        {"Igrave", 0x00CC}, {"Iacute", 0x00CD}, {"Icircumflex", 0x00CE}, {"Idieresis", 0x00CF},
        {"Eth", 0x00D0}, {"Ntilde", 0x00D1}, {"Ograve", 0x00D2}, {"Oacute", 0x00D3},
        {"Ocircumflex", 0x00D4}, {"Otilde", 0x00D5}, {"Odieresis", 0x00D6}, {"multiply", 0x00D7},
        {"Oslash", 0x00D8}, {"Ugrave", 0x00D9}, {"Uacute", 0x00DA}, {"Ucircumflex", 0x00DB},
        {"Udieresis", 0x00DC}, {"Yacute", 0x00DD}, {"Thorn", 0x00DE}, {"germandbls", 0x00DF},
        {"agrave", 0x00E0}, {"aacute", 0x00E1}, {"acircumflex", 0x00E2}, {"atilde", 0x00E3},
        {"adieresis", 0x00E4}, {"aring", 0x00E5}, {"ae", 0x00E6}, {"ccedilla", 0x00E7},
        {"egrave", 0x00E8}, {"eacute", 0x00E9}, {"ecircumflex", 0x00EA}, {"edieresis", 0x00EB},
        {"igrave", 0x00EC}, {"iacute", 0x00ED}, {"icircumflex", 0x00EE}, {"idieresis", 0x00EF},
        {"eth", 0x00F0}, {"ntilde", 0x00F1}, {"ograve", 0x00F2}, {"oacute", 0x00F3},
        {"ocircumflex", 0x00F4}, {"otilde", 0x00F5}, {"odieresis", 0x00F6}, {"divide", 0x00F7},
        {"oslash", 0x00F8}, {"ugrave", 0x00F9}, {"uacute", 0x00FA}, {"ucircumflex", 0x00FB},
        {"udieresis", 0x00FC}, {"yacute", 0x00FD}, {"thorn", 0x00FE}, {"ydieresis", 0x00FF},
        {"OE", 0x0152}, {"oe", 0x0153}, {"Scaron", 0x0160}, {"scaron", 0x0161},
        {"Ydieresis", 0x0178}, {"Zcaron", 0x017D}, {"zcaron", 0x017E}, {"florin", 0x0192},
        {"circumflex", 0x02C6}, {"tilde", 0x02DC}, {"endash", 0x2013}, {"emdash", 0x2014},
        {"quoteleft", 0x2018}, {"quoteright", 0x2019}, {"quotesinglbase", 0x201A},
        {"quotedblleft", 0x201C}, {"quotedblright", 0x201D}, {"quotedblbase", 0x201E},
        {"dagger", 0x2020}, {"daggerdbl", 0x2021}, {"bullet", 0x2022}, {"ellipsis", 0x2026},
        {"perthousand", 0x2030}, {"guilsinglleft", 0x2039}, {"guilsinglright", 0x203A},
        {"fraction", 0x2044}, {"Euro", 0x20AC}, {"trademark", 0x2122},
        {"ff", 0xFB00}, {"fi", 0xFB01}, {"fl", 0xFB02}, {"ffi", 0xFB03}, {"ffl", 0xFB04},
        {"Alpha", 0x0391}, {"Beta", 0x0392}, {"Gamma", 0x0393}, {"Delta", 0x0394},
        {"Epsilon", 0x0395}, {"Zeta", 0x0396}, {"Eta", 0x0397}, {"Theta", 0x0398},
        {"Lambda", 0x039B}, {"Pi", 0x03A0}, {"Sigma", 0x03A3}, {"Phi", 0x03A6},
        {"Psi", 0x03A8}, {"Omega", 0x03A9},
        {"alpha", 0x03B1}, {"beta", 0x03B2}, {"gamma", 0x03B3}, {"delta", 0x03B4},
        {"epsilon", 0x03B5}, {"zeta", 0x03B6}, {"eta", 0x03B7}, {"theta", 0x03B8},
        {"lambda", 0x03BB}, {"pi", 0x03C0}, {"sigma", 0x03C3}, {"tau", 0x03C4},
        {"phi", 0x03C6}, {"psi", 0x03C8}, {"omega", 0x03C9},
        {"partialdiff", 0x2202}, {"summation", 0x2211}, {"product", 0x220F},
        {"radical", 0x221A}, {"infinity", 0x221E}, {"integral", 0x222B},
        {"approxequal", 0x2248}, {"notequal", 0x2260}, {"lessequal", 0x2264}, {"greaterequal", 0x2265}
    };

    for (const auto &entry : kGlyphMap) {
        if (name == entry.name) {
            return entry.cp;
        }
    }
    return 0;
}

int pdfStandardFontCharWidth(const std::string &baseFont, uint32_t unicodeCp) {
    // Monospace Courier family: always 600/1000 em
    if (baseFont.find("Courier") != std::string::npos || baseFont.find("Mono") != std::string::npos) {
        return 600;
    }
    // Proportional Helvetica / Times approximation in 1/1000 em units for ASCII
    static const uint16_t kHelveticaWidths[95] = {
        278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278,
        556, 556, 556, 556, 556, 556, 556, 556, 556, 556, 278, 278, 584, 584, 584, 556,
        1015,667, 667, 722, 722, 667, 611, 778, 722, 278, 500, 667, 556, 833, 722, 778,
        667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 278, 278, 278, 469, 556,
        333, 556, 556, 500, 556, 556, 278, 556, 556, 222, 222, 500, 222, 833, 556, 556,
        556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500, 334, 260, 334, 584
    };
    if (unicodeCp >= 32 && unicodeCp <= 126) {
        return kHelveticaWidths[unicodeCp - 32];
    }
    if (unicodeCp >= 0x3000) {
        return 1000; // CJK fullwidth
    }
    return 556;
}

// =============================================================================
// 3. PdfCanvas: 2D Anti-Aliased Vector Path, Scanline & Affine Image Engine
// =============================================================================

PdfCanvas::PdfCanvas(uint8_t *grayBuf, int width, int height)
    : m_buf(grayBuf),
      m_width(width),
      m_height(height),
      m_clipX0(0),
      m_clipY0(0),
      m_clipX1(width),
      m_clipY1(height) {}

void PdfCanvas::setClipRect(int x0, int y0, int x1, int y1) {
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    m_clipX0 = std::max(0, std::min(m_width, x0));
    m_clipY0 = std::max(0, std::min(m_height, y0));
    m_clipX1 = std::max(0, std::min(m_width, x1));
    m_clipY1 = std::max(0, std::min(m_height, y1));
}

void PdfCanvas::resetClipRect() {
    m_clipX0 = 0;
    m_clipY0 = 0;
    m_clipX1 = m_width;
    m_clipY1 = m_height;
}

void PdfCanvas::getClipRect(int &x0, int &y0, int &x1, int &y1) const {
    x0 = m_clipX0;
    y0 = m_clipY0;
    x1 = m_clipX1;
    y1 = m_clipY1;
}

void PdfCanvas::intersectClipPathBBox() {
    if (m_subpaths.empty()) return;
    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    bool any = false;
    for (const auto &sp : m_subpaths) {
        for (const auto &pt : sp.pts) {
            minX = std::min(minX, pt.x);
            minY = std::min(minY, pt.y);
            maxX = std::max(maxX, pt.x);
            maxY = std::max(maxY, pt.y);
            any = true;
        }
    }
    if (!any) return;
    int bx0 = static_cast<int>(std::floor(minX));
    int by0 = static_cast<int>(std::floor(minY));
    int bx1 = static_cast<int>(std::ceil(maxX));
    int by1 = static_cast<int>(std::ceil(maxY));
    if (bx1 - bx0 < 2 || by1 - by0 < 2) {
        // Ignore degenerate zero-area clip paths so they do not blank the page
        return;
    }
    setClipRect(std::max(m_clipX0, bx0), std::max(m_clipY0, by0),
                std::min(m_clipX1, bx1), std::min(m_clipY1, by1));
}

void PdfCanvas::clearPath() {
    m_subpaths.clear();
    m_hasCurrentPoint = false;
}

void PdfCanvas::moveTo(float x, float y) {
    Subpath sp;
    sp.pts.push_back({ x, y });
    sp.closed = false;
    m_subpaths.push_back(std::move(sp));
    m_curX = m_startX = x;
    m_curY = m_startY = y;
    m_hasCurrentPoint = true;
}

void PdfCanvas::lineTo(float x, float y) {
    if (!m_hasCurrentPoint || m_subpaths.empty()) {
        moveTo(x, y);
        return;
    }
    m_subpaths.back().pts.push_back({ x, y });
    m_curX = x;
    m_curY = y;
}

void PdfCanvas::flattenQuadRecursive(float x0, float y0, float x1, float y1, float x2, float y2, int depth) {
    // Check chord flatness error
    float mx = 0.5f * (x0 + x2);
    float my = 0.5f * (y0 + y2);
    float dx = x1 - mx;
    float dy = y1 - my;
    if (depth >= 7 || (dx * dx + dy * dy) <= 0.16f) {
        lineTo(x2, y2);
        return;
    }
    float x01 = 0.5f * (x0 + x1);
    float y01 = 0.5f * (y0 + y1);
    float x12 = 0.5f * (x1 + x2);
    float y12 = 0.5f * (y1 + y2);
    float x012 = 0.5f * (x01 + x12);
    float y012 = 0.5f * (y01 + y12);
    flattenQuadRecursive(x0, y0, x01, y01, x012, y012, depth + 1);
    flattenQuadRecursive(x012, y012, x12, y12, x2, y2, depth + 1);
}

void PdfCanvas::quadTo(float x1, float y1, float x2, float y2) {
    if (!m_hasCurrentPoint) {
        moveTo(x1, y1);
    }
    flattenQuadRecursive(m_curX, m_curY, x1, y1, x2, y2, 0);
}

void PdfCanvas::flattenCubicRecursive(
    float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, int depth)
{
    float ux = 3.0f * x1 - 2.0f * x0 - x3;
    float uy = 3.0f * y1 - 2.0f * y0 - y3;
    float vx = 3.0f * x2 - 2.0f * x3 - x0;
    float vy = 3.0f * y2 - 2.0f * y3 - y0;
    float errX = std::max(ux * ux, vx * vx);
    float errY = std::max(uy * uy, vy * vy);
    if (depth >= 7 || (errX + errY) <= 1.2f) {
        lineTo(x3, y3);
        return;
    }
    float x01 = 0.5f * (x0 + x1), y01 = 0.5f * (y0 + y1);
    float x12 = 0.5f * (x1 + x2), y12 = 0.5f * (y1 + y2);
    float x23 = 0.5f * (x2 + x3), y23 = 0.5f * (y2 + y3);
    float x012 = 0.5f * (x01 + x12), y012 = 0.5f * (y01 + y12);
    float x123 = 0.5f * (x12 + x23), y123 = 0.5f * (y12 + y23);
    float x0123 = 0.5f * (x012 + x123), y0123 = 0.5f * (y012 + y123);
    flattenCubicRecursive(x0, y0, x01, y01, x012, y012, x0123, y0123, depth + 1);
    flattenCubicRecursive(x0123, y0123, x123, y123, x23, y23, x3, y3, depth + 1);
}

void PdfCanvas::cubicTo(float x1, float y1, float x2, float y2, float x3, float y3) {
    if (!m_hasCurrentPoint) {
        moveTo(x1, y1);
    }
    flattenCubicRecursive(m_curX, m_curY, x1, y1, x2, y2, x3, y3, 0);
}

void PdfCanvas::rect(float x, float y, float w, float h) {
    moveTo(x, y);
    lineTo(x + w, y);
    lineTo(x + w, y + h);
    lineTo(x, y + h);
    closePath();
}

void PdfCanvas::closePath() {
    if (!m_subpaths.empty() && !m_subpaths.back().pts.empty()) {
        Subpath &sp = m_subpaths.back();
        if (sp.pts.back().x != sp.pts.front().x || sp.pts.back().y != sp.pts.front().y) {
            sp.pts.push_back(sp.pts.front());
        }
        sp.closed = true;
        m_curX = m_startX = sp.pts.front().x;
        m_curY = m_startY = sp.pts.front().y;
    }
}

void PdfCanvas::fillPath(uint8_t fillGray, bool evenOdd, float coverageBoost) {
    if (!m_buf || m_subpaths.empty() || m_clipX0 >= m_clipX1 || m_clipY0 >= m_clipY1) {
        return;
    }

    struct Edge {
        float x0, y0;
        float x1, y1;
        int dir; // +1 downward, -1 upward
    };

    std::vector<Edge> edges;
    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;

    for (const auto &sp : m_subpaths) {
        if (sp.pts.size() < 2) continue;
        size_t n = sp.pts.size();
        for (size_t i = 0; i < n; ++i) {
            const Point &p0 = sp.pts[i];
            const Point &p1 = (i + 1 < n) ? sp.pts[i + 1] : sp.pts[0];
            if (i + 1 == n && (p0.x == p1.x && p0.y == p1.y)) continue;
            minX = std::min({ minX, p0.x, p1.x });
            minY = std::min({ minY, p0.y, p1.y });
            maxX = std::max({ maxX, p0.x, p1.x });
            maxY = std::max({ maxY, p0.y, p1.y });

            if (std::fabs(p0.y - p1.y) < 1e-5f) continue;
            if (p0.y < p1.y) {
                edges.push_back({ p0.x, p0.y, p1.x, p1.y, +1 });
            } else {
                edges.push_back({ p1.x, p1.y, p0.x, p0.y, -1 });
            }
        }
    }

    if (edges.empty()) return;

    int yStart = std::max(m_clipY0, static_cast<int>(std::floor(minY)));
    int yEnd   = std::min(m_clipY1, static_cast<int>(std::ceil(maxY)) + 1);
    int xStart = std::max(m_clipX0, static_cast<int>(std::floor(minX)));
    int xEnd   = std::min(m_clipX1, static_cast<int>(std::ceil(maxX)) + 1);
    if (yStart >= yEnd || xStart >= xEnd) return;

    const int spanW = xEnd - xStart;
    std::vector<uint8_t> covRow(static_cast<size_t>(spanW), 0);

    struct Crossing {
        float x;
        int dir;
        bool operator<(const Crossing &o) const { return x < o.x; }
    };
    std::vector<Crossing> crossings;
    crossings.reserve(edges.size());

    // 4x vertical supersampling per pixel row for crisp anti-aliased edges
    static const float kSubOffsets[4] = { 0.125f, 0.375f, 0.625f, 0.875f };

    for (int y = yStart; y < yEnd; ++y) {
        std::fill(covRow.begin(), covRow.end(), 0);
        bool anyCov = false;

        for (int sub = 0; sub < 4; ++sub) {
            float scanY = static_cast<float>(y) + kSubOffsets[sub];
            crossings.clear();

            for (const auto &e : edges) {
                if (scanY >= e.y0 && scanY < e.y1) {
                    float t = (scanY - e.y0) / (e.y1 - e.y0);
                    float ix = e.x0 + t * (e.x1 - e.x0);
                    crossings.push_back({ ix, e.dir });
                }
            }
            if (crossings.size() < 2) continue;
            std::sort(crossings.begin(), crossings.end());

            int winding = 0;
            for (size_t i = 0; i + 1 < crossings.size(); ++i) {
                winding += crossings[i].dir;
                bool inside = evenOdd ? ((i & 1) == 0) : (winding != 0);
                if (!inside) continue;

                float fx0 = crossings[i].x;
                float fx1 = crossings[i + 1].x;
                if (fx1 <= fx0) continue;

                int px0 = std::max(xStart, static_cast<int>(std::floor(fx0)));
                int px1 = std::min(xEnd, static_cast<int>(std::ceil(fx1)));
                for (int px = px0; px < px1; ++px) {
                    float left = std::max(fx0, static_cast<float>(px));
                    float right = std::min(fx1, static_cast<float>(px + 1));
                    if (right > left) {
                        int add = static_cast<int>((right - left) * 63.75f + 0.5f);
                        int cur = covRow[px - xStart] + add;
                        covRow[px - xStart] = static_cast<uint8_t>(std::min(255, cur));
                        anyCov = true;
                    }
                }
            }
        }

        if (anyCov) {
            const bool isGlyphFill = (coverageBoost > 1.2f);
            const uint8_t snapThresh = (coverageBoost > 2.0f) ? 46 : 56;
            if (isGlyphFill) {
                // First pass for glyph strokes over mid-tone/photo backgrounds (24 < dst < 232):
                // add a 1px contrast halo (pure white 255 around black ink, pure black 0 around white ink)
                // so text over dithered photos stands out cleanly and is never eaten by error diffusion.
                const uint8_t haloGray = (fillGray < 128) ? 255 : 0;
                for (int i = 0; i < spanW; ++i) {
                    if (covRow[i] >= snapThresh) {
                        int px = xStart + i;
                        const int dx[4] = { -1, 1, 0, 0 };
                        const int dy[4] = { 0, 0, -1, 1 };
                        for (int k = 0; k < 4; ++k) {
                            int nx = px + dx[k];
                            int ny = y + dy[k];
                            if (nx >= m_clipX0 && nx < m_clipX1 && ny >= m_clipY0 && ny < m_clipY1) {
                                uint8_t &ndst = m_buf[ny * m_width + nx];
                                if (ndst > 24 && ndst < 232) {
                                    ndst = haloGray;
                                }
                            }
                        }
                    }
                }
            }
            for (int i = 0; i < spanW; ++i) {
                if (covRow[i] > 0) {
                    uint8_t alpha = covRow[i];
                    if (isGlyphFill) {
                        int boosted = static_cast<int>(static_cast<float>(alpha) * coverageBoost + 0.5f);
                        alpha = (alpha >= snapThresh) ? 255 : static_cast<uint8_t>(std::min(255, boosted));
                    }
                    blendPixel(xStart + i, y, fillGray, alpha);
                }
            }
        }
    }
}

void PdfCanvas::strokeSegmentAA(float x0, float y0, float x1, float y1, uint8_t gray, float width) {
    float dx = x1 - x0;
    float dy = y1 - y0;
    float len = std::hypot(dx, dy);
    if (len < 0.05f) {
        blendPixel(static_cast<int>(x0 + 0.5f), static_cast<int>(y0 + 0.5f), gray, 255);
        return;
    }

    float halfW = std::max(0.5f, width * 0.5f);
    int minX = std::max(m_clipX0, static_cast<int>(std::floor(std::min(x0, x1) - halfW - 1.0f)));
    int maxX = std::min(m_clipX1, static_cast<int>(std::ceil(std::max(x0, x1) + halfW + 1.0f)));
    int minY = std::max(m_clipY0, static_cast<int>(std::floor(std::min(y0, y1) - halfW - 1.0f)));
    int maxY = std::min(m_clipY1, static_cast<int>(std::ceil(std::max(y0, y1) + halfW + 1.0f)));

    float invLen2 = 1.0f / (len * len);
    for (int py = minY; py < maxY; ++py) {
        float cy = static_cast<float>(py) + 0.5f;
        for (int px = minX; px < maxX; ++px) {
            float cx = static_cast<float>(px) + 0.5f;
            float t = ((cx - x0) * dx + (cy - y0) * dy) * invLen2;
            t = std::max(0.0f, std::min(1.0f, t));
            float projX = x0 + t * dx;
            float projY = y0 + t * dy;
            float dist = std::hypot(cx - projX, cy - projY);
            float cov = (halfW + 0.5f) - dist;
            if (cov > 0.0f) {
                uint8_t alpha = (cov >= 1.0f) ? 255 : static_cast<uint8_t>(cov * 255.0f);
                blendPixel(px, py, gray, alpha);
            }
        }
    }
}

void PdfCanvas::strokePath(
    uint8_t strokeGray,
    float lineWidth,
    const std::vector<float> &dashArray,
    float dashPhase)
{
    if (!m_buf || m_subpaths.empty()) return;
    float effW = std::max(0.8f, lineWidth);

    bool hasDash = false;
    float dashPeriod = 0.0f;
    for (float d : dashArray) {
        if (d > 0.1f) {
            hasDash = true;
            dashPeriod += d;
        }
    }

    for (const auto &sp : m_subpaths) {
        if (sp.pts.size() < 2) continue;
        float distAlong = std::fmod(std::max(0.0f, dashPhase), std::max(1.0f, dashPeriod));

        for (size_t i = 0; i + 1 < sp.pts.size(); ++i) {
            const Point &p0 = sp.pts[i];
            const Point &p1 = sp.pts[i + 1];
            if (!hasDash) {
                strokeSegmentAA(p0.x, p0.y, p1.x, p1.y, strokeGray, effW);
            } else {
                float segLen = std::hypot(p1.x - p0.x, p1.y - p0.y);
                if (segLen < 0.05f) continue;
                float pos = 0.0f;
                while (pos < segLen) {
                    float remInPeriod = std::fmod(distAlong + pos, dashPeriod);
                    size_t dIdx = 0;
                    float accum = 0.0f;
                    for (; dIdx < dashArray.size(); ++dIdx) {
                        if (remInPeriod < accum + dashArray[dIdx]) break;
                        accum += dashArray[dIdx];
                    }
                    float stepRem = (dIdx < dashArray.size())
                        ? (accum + dashArray[dIdx] - remInPeriod)
                        : (segLen - pos);
                    stepRem = std::max(0.5f, std::min(segLen - pos, stepRem));

                    if ((dIdx & 1) == 0) {
                        float t0 = pos / segLen;
                        float t1 = (pos + stepRem) / segLen;
                        strokeSegmentAA(
                            p0.x + t0 * (p1.x - p0.x),
                            p0.y + t0 * (p1.y - p0.y),
                            p0.x + t1 * (p1.x - p0.x),
                            p0.y + t1 * (p1.y - p0.y),
                            strokeGray,
                            effW);
                    }
                    pos += stepRem;
                }
                distAlong += segLen;
            }
        }
    }
}

void PdfCanvas::drawImageAffine(
    const uint8_t *srcGray,
    int srcW,
    int srcH,
    const float imgToCanvas[6],
    bool isStencilMask,
    uint8_t maskInkGray)
{
    if (!m_buf || !srcGray || srcW <= 0 || srcH <= 0) return;

    // Corners of unit square [0,1]x[0,1] in canvas pixels
    const float cornersU[4] = { 0.0f, 1.0f, 1.0f, 0.0f };
    const float cornersV[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    for (int i = 0; i < 4; ++i) {
        float cx = imgToCanvas[0] * cornersU[i] + imgToCanvas[2] * cornersV[i] + imgToCanvas[4];
        float cy = imgToCanvas[1] * cornersU[i] + imgToCanvas[3] * cornersV[i] + imgToCanvas[5];
        minX = std::min(minX, cx);
        minY = std::min(minY, cy);
        maxX = std::max(maxX, cx);
        maxY = std::max(maxY, cy);
    }

    int x0 = std::max(m_clipX0, static_cast<int>(std::floor(minX)));
    int y0 = std::max(m_clipY0, static_cast<int>(std::floor(minY)));
    int x1 = std::min(m_clipX1, static_cast<int>(std::ceil(maxX)));
    int y1 = std::min(m_clipY1, static_cast<int>(std::ceil(maxY)));
    if (x0 >= x1 || y0 >= y1) return;

    float det = imgToCanvas[0] * imgToCanvas[3] - imgToCanvas[1] * imgToCanvas[2];
    if (std::fabs(det) < 1e-6f) return;
    float invDet = 1.0f / det;
    float ia =  imgToCanvas[3] * invDet;
    float ib = -imgToCanvas[1] * invDet;
    float ic = -imgToCanvas[2] * invDet;
    float id =  imgToCanvas[0] * invDet;

    for (int py = y0; py < y1; ++py) {
        float dy = (static_cast<float>(py) + 0.5f) - imgToCanvas[5];
        uint8_t *dstRow = m_buf + py * m_width;
        for (int px = x0; px < x1; ++px) {
            float dx = (static_cast<float>(px) + 0.5f) - imgToCanvas[4];
            float u = ia * dx + ic * dy;
            float v = ib * dx + id * dy;
            if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) continue;

            // In PDF image space, u in [0,1] maps left-to-right (0..srcW-1),
            // and v in [0,1] maps bottom-to-top (srcH-1..0).
            float sx = u * static_cast<float>(srcW - 1);
            float sy = (1.0f - v) * static_cast<float>(srcH - 1);

            int ix0 = std::max(0, std::min(srcW - 1, static_cast<int>(sx)));
            int iy0 = std::max(0, std::min(srcH - 1, static_cast<int>(sy)));
            int ix1 = std::min(srcW - 1, ix0 + 1);
            int iy1 = std::min(srcH - 1, iy0 + 1);
            float fx = sx - static_cast<float>(ix0);
            float fy = sy - static_cast<float>(iy0);

            float p00 = srcGray[iy0 * srcW + ix0];
            float p10 = srcGray[iy0 * srcW + ix1];
            float p01 = srcGray[iy1 * srcW + ix0];
            float p11 = srcGray[iy1 * srcW + ix1];
            float top = p00 + fx * (p10 - p00);
            float bot = p01 + fx * (p11 - p01);
            uint8_t val = static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, top + fy * (bot - top) + 0.5f)));

            if (isStencilMask) {
                // For stencil masks, preserve thin 1px typography strokes during downscaling
                float minTex = std::min(std::min(p00, p10), std::min(p01, p11));
                uint8_t maskVal = static_cast<uint8_t>(std::min<float>(val, 0.5f * (val + minTex)));
                uint8_t alpha = 255 - maskVal;
                if (alpha > 0) {
                    blendPixel(px, py, maskInkGray, alpha);
                }
            } else {
                // Preserve existing dark ink when image pixel is near-white (e.g. transparent/white background)
                if (val > 242 && dstRow[px] < val) {
                    continue;
                }
                dstRow[px] = val;
            }
        }
    }
}

// =============================================================================
// 4. PdfTrueTypeFont: Embedded /FontFile2 (TTF) & /FontFile3 (CFF/Type1C) Rasterizer
// =============================================================================

namespace {

// Standard 391 CFF Strings (SID 0..390) per Adobe Technical Note #5176
static const char *const kCffStandardStrings[391] = {
    ".notdef", "space", "exclam", "quotedbl", "numbersign", "dollar", "percent", "ampersand",
    "quoteright", "parenleft", "parenright", "asterisk", "plus", "comma", "hyphen", "period",
    "slash", "zero", "one", "two", "three", "four", "five", "six",
    "seven", "eight", "nine", "colon", "semicolon", "less", "equal", "greater",
    "question", "at", "A", "B", "C", "D", "E", "F",
    "G", "H", "I", "J", "K", "L", "M", "N",
    "O", "P", "Q", "R", "S", "T", "U", "V",
    "W", "X", "Y", "Z", "bracketleft", "backslash", "bracketright", "asciicircum",
    "underscore", "quoteleft", "a", "b", "c", "d", "e", "f",
    "g", "h", "i", "j", "k", "l", "m", "n",
    "o", "p", "q", "r", "s", "t", "u", "v",
    "w", "x", "y", "z", "braceleft", "bar", "braceright", "asciitilde",
    "exclamdown", "cent", "sterling", "fraction", "yen", "florin", "section", "currency",
    "quotesingle", "quotedblleft", "guillemotleft", "guilsinglleft", "guilsinglright", "fi", "fl", "endash",
    "dagger", "daggerdbl", "periodcentered", "paragraph", "bullet", "quotesinglbase", "quotedblbase", "quotedblright",
    "guillemotright", "ellipsis", "perthousand", "questiondown", "grave", "acute", "circumflex", "tilde",
    "macron", "breve", "dotaccent", "dieresis", "ring", "cedilla", "hungarumlaut", "ogonek",
    "caron", "emdash", "AE", "ordfeminine", "Lslash", "Oslash", "OE", "ordmasculine",
    "ae", "dotlessi", "lslash", "oslash", "oe", "germandbls", "onesuperior", "logicalnot",
    "mu", "trademark", "Eth", "onehalf", "plusminus", "Thorn", "onequarter", "divide",
    "brokenbar", "degree", "thorn", "threequarters", "twosuperior", "registered", "minus", "eth",
    "multiply", "threesuperior", "copyright", "Aacute", "Acircumflex", "Adieresis", "Agrave", "Aring",
    "Atilde", "Ccedilla", "Eacute", "Ecircumflex", "Edieresis", "Egrave", "Iacute", "Icircumflex",
    "Idieresis", "Igrave", "Ntilde", "Oacute", "Ocircumflex", "Odieresis", "Ograve", "Otilde",
    "Scaron", "Uacute", "Ucircumflex", "Udieresis", "Ugrave", "Yacute", "Ydieresis", "Zcaron",
    "aacute", "acircumflex", "adieresis", "agrave", "aring", "atilde", "ccedilla", "eacute",
    "ecircumflex", "edieresis", "egrave", "iacute", "icircumflex", "idieresis", "igrave", "ntilde",
    "oacute", "ocircumflex", "odieresis", "ograve", "otilde", "scaron", "uacute", "ucircumflex",
    "udieresis", "ugrave", "yacute", "ydieresis", "zcaron", "exclamsmall", "Hungarumlautsmall", "dollaroldstyle",
    "dollarsuperior", "ampersandsmall", "Acutesmall", "parenleftsuperior", "parenrightsuperior", "twodotenleader", "onedotenleader", "zerooldstyle",
    "oneoldstyle", "twooldstyle", "threeoldstyle", "fouroldstyle", "fiveoldstyle", "sixoldstyle", "sevenoldstyle", "eightoldstyle",
    "nineoldstyle", "commasuperior", "threequartersemdash", "periodsuperior", "questionsmall", "asuperior", "bsuperior", "centsuperior",
    "dsuperior", "esuperior", "isuperior", "lsuperior", "msuperior", "nsuperior", "osuperior", "rsuperior",
    "ssuperior", "tsuperior", "ff", "ffi", "ffl", "parenleftinferior", "parenrightinferior", "Circumflexsmall",
    "hyphensuperior", "Gravesmall", "Asmall", "Bsmall", "Csmall", "Dsmall", "Esmall", "Fsmall",
    "Gsmall", "Hsmall", "Ismall", "Jsmall", "Ksmall", "Lsmall", "Msmall", "Nsmall",
    "Osmall", "Psmall", "Qsmall", "Rsmall", "Ssmall", "Tsmall", "Usmall", "Vsmall",
    "Wsmall", "Xsmall", "Ysmall", "Zsmall", "colonmonetary", "onefitted", "rupiah", "Tildesmall",
    "exclamdownsmall", "centoldstyle", "Lslashsmall", "Scaronsmall", "Zcaronsmall", "Dieresissmall", "Brevesmall", "Caronsmall",
    "Dotaccentsmall", "Macronsmall", "figuredash", "hypheninferior", "Ogoneksmall", "Ringsmall", "Cedillasmall", "questiondownsmall",
    "oneeighth", "threeeighths", "fiveeighths", "seveneighths", "onethird", "twothirds", "zerosuperior", "foursuperior",
    "fivesuperior", "sixsuperior", "sevensuperior", "eightsuperior", "ninesuperior", "zeroinferior", "oneinferior", "twoinferior",
    "threeinferior", "fourinferior", "fiveinferior", "sixinferior", "seveninferior", "eightinferior", "nineinferior", "centinferior",
    "dollarinferior", "periodinferior", "commainferior", "Agravesmall", "Aacutesmall", "Acircumflexsmall", "Atildesmall", "Adieresissmall",
    "Aringsmall", "AEsmall", "Ccedillasmall", "Egravesmall", "Eacutesmall", "Ecircumflexsmall", "Edieresissmall", "Igravesmall",
    "Iacutesmall", "Icircumflexsmall", "Idieresissmall", "Ethsmall", "Ntildesmall", "Ogravesmall", "Oacutesmall", "Ocircumflexsmall",
    "Otildesmall", "Odieresissmall", "OEsmall", "Oslashsmall", "Ugravesmall", "Uacutesmall", "Ucircumflexsmall", "Udieresissmall",
    "Yacutesmall", "Thornsmall", "Ydieresissmall", "001.000", "001.001", "001.002", "001.003", "Black",
    "Bold", "Book", "Light", "Medium", "Regular", "Roman", "Semibold"
};

static uint32_t readCffOffset(const uint8_t *p, uint8_t offSize) {
    uint32_t v = 0;
    for (uint8_t i = 0; i < offSize; ++i) {
        v = (v << 8) | p[i];
    }
    return v;
}

static bool readCffIndex(
    const uint8_t *cff,
    uint32_t cffLen,
    uint32_t pos,
    PdfTrueTypeFont::CffIndex &outIdx,
    uint32_t &outNextPos)
{
    outIdx.dataStart = 0;
    outIdx.offsets.clear();
    if (pos + 2 > cffLen) return false;
    uint16_t count = readU16BE(cff + pos);
    if (count == 0) {
        outNextPos = pos + 2;
        return true;
    }
    if (pos + 3 > cffLen) return false;
    uint8_t offSize = cff[pos + 2];
    if (offSize < 1 || offSize > 4) return false;
    uint32_t offTableLen = static_cast<uint32_t>(count + 1) * offSize;
    uint32_t dataStart = pos + 3 + offTableLen;
    if (dataStart > cffLen) return false;

    outIdx.dataStart = dataStart;
    outIdx.offsets.resize(count + 1);
    const uint8_t *op = cff + pos + 3;
    for (uint32_t i = 0; i <= count; ++i) {
        uint32_t rawOff = readCffOffset(op + i * offSize, offSize);
        if (rawOff < 1) return false;
        uint32_t relOff = rawOff - 1;
        if (dataStart + relOff > cffLen) return false;
        if (i > 0 && relOff < outIdx.offsets[i - 1]) return false;
        outIdx.offsets[i] = relOff;
    }
    outNextPos = dataStart + outIdx.offsets[count];
    return true;
}

static void parseCffDict(
    const uint8_t *dict,
    uint32_t dictLen,
    std::unordered_map<int, std::vector<float>> &outOps)
{
    std::vector<float> args;
    uint32_t i = 0;
    while (i < dictLen) {
        uint8_t b0 = dict[i++];
        if (b0 <= 21) {
            int op = b0;
            if (b0 == 12) {
                if (i >= dictLen) break;
                op = 1200 + dict[i++];
            }
            outOps[op] = args;
            args.clear();
        } else if (b0 == 28) {
            if (i + 2 > dictLen) break;
            int16_t v = static_cast<int16_t>((dict[i] << 8) | dict[i + 1]);
            i += 2;
            args.push_back(static_cast<float>(v));
        } else if (b0 == 29) {
            if (i + 4 > dictLen) break;
            int32_t v = static_cast<int32_t>(
                (static_cast<uint32_t>(dict[i]) << 24) |
                (static_cast<uint32_t>(dict[i + 1]) << 16) |
                (static_cast<uint32_t>(dict[i + 2]) << 8) |
                static_cast<uint32_t>(dict[i + 3]));
            i += 4;
            args.push_back(static_cast<float>(v));
        } else if (b0 == 30) {
            char buf[64];
            int bi = 0;
            bool done = false;
            while (i < dictLen && !done) {
                uint8_t byte = dict[i++];
                for (int nibIdx = 0; nibIdx < 2; ++nibIdx) {
                    uint8_t nib = (nibIdx == 0) ? (byte >> 4) : (byte & 0x0F);
                    if (nib <= 9) {
                        if (bi + 1 < static_cast<int>(sizeof(buf))) buf[bi++] = static_cast<char>('0' + nib);
                    } else if (nib == 0x0A) {
                        if (bi + 1 < static_cast<int>(sizeof(buf))) buf[bi++] = '.';
                    } else if (nib == 0x0B) {
                        if (bi + 1 < static_cast<int>(sizeof(buf))) buf[bi++] = 'E';
                    } else if (nib == 0x0C) {
                        if (bi + 2 < static_cast<int>(sizeof(buf))) { buf[bi++] = 'E'; buf[bi++] = '-'; }
                    } else if (nib == 0x0E) {
                        if (bi + 1 < static_cast<int>(sizeof(buf))) buf[bi++] = '-';
                    } else if (nib == 0x0F) {
                        done = true;
                        break;
                    }
                }
            }
            buf[bi] = '\0';
            args.push_back(strtof(buf, nullptr));
        } else if (b0 >= 32 && b0 <= 246) {
            args.push_back(static_cast<float>(static_cast<int>(b0) - 139));
        } else if (b0 >= 247 && b0 <= 250) {
            if (i >= dictLen) break;
            uint8_t b1 = dict[i++];
            args.push_back(static_cast<float>((static_cast<int>(b0) - 247) * 256 + static_cast<int>(b1) + 108));
        } else if (b0 >= 251 && b0 <= 254) {
            if (i >= dictLen) break;
            uint8_t b1 = dict[i++];
            args.push_back(static_cast<float>(-(static_cast<int>(b0) - 251) * 256 - static_cast<int>(b1) - 108));
        }
    }
}

static int computeCffSubrBias(size_t nSubrs) {
    if (nSubrs < 1240) return 107;
    if (nSubrs < 33900) return 1131;
    return 32768;
}

} // namespace

bool PdfTrueTypeFont::parseCff(uint32_t cffOffset, uint32_t cffLen) {
    if (cffLen < 16 || static_cast<uint64_t>(cffOffset) + cffLen > m_data.size()) return false;
    const uint8_t *cff = m_data.data() + cffOffset;

    uint8_t major = cff[0];
    uint8_t hdrSize = cff[2];
    if (major != 1 || hdrSize < 4 || hdrSize >= cffLen) return false;

    m_cffOffset = cffOffset;
    m_cffLen = cffLen;

    CffIndex nameIdx, topDictIdx, stringIdx;
    uint32_t pos = hdrSize;
    if (!readCffIndex(cff, cffLen, pos, nameIdx, pos)) return false;
    if (!readCffIndex(cff, cffLen, pos, topDictIdx, pos) || topDictIdx.count() == 0) return false;
    if (!readCffIndex(cff, cffLen, pos, stringIdx, pos)) return false;
    if (!readCffIndex(cff, cffLen, pos, m_cffGlobalSubrs, pos)) return false;

    uint32_t tdStart = topDictIdx.dataStart + topDictIdx.offsets[0];
    uint32_t tdLen = topDictIdx.offsets[1] - topDictIdx.offsets[0];
    std::unordered_map<int, std::vector<float>> topOps;
    parseCffDict(cff + tdStart, tdLen, topOps);

    // FontMatrix (operator 1207)
    auto fmIt = topOps.find(1207);
    if (fmIt != topOps.end() && fmIt->second.size() >= 4) {
        float sx = std::fabs(fmIt->second[0]);
        if (sx > 1e-6f) {
            m_unitsPerEm = std::max(100, static_cast<int>(std::round(1.0f / sx)));
        }
    }

    // ROS (operator 1230) indicates CID-keyed CFF font
    m_cffIsCid = (topOps.find(1230) != topOps.end());

    // CharStrings INDEX (operator 17)
    auto csIt = topOps.find(17);
    if (csIt == topOps.end() || csIt->second.empty()) return false;
    uint32_t csOff = static_cast<uint32_t>(std::max(0.0f, csIt->second.back()));
    uint32_t dummyNext = 0;
    if (csOff == 0 || csOff >= cffLen || !readCffIndex(cff, cffLen, csOff, m_cffCharStrings, dummyNext)) {
        return false;
    }
    m_numGlyphs = static_cast<int>(m_cffCharStrings.count());
    if (m_numGlyphs <= 0) return false;

    // Private DICT (operator 18: size, offset) & Local Subrs (operator 19 relative to Private DICT)
    auto parsePrivateSubrs = [&](uint32_t privSize, uint32_t privOff, CffIndex &outSubrs) {
        if (privSize == 0 || privOff == 0 || static_cast<uint64_t>(privOff) + privSize > cffLen) return;
        std::unordered_map<int, std::vector<float>> privOps;
        parseCffDict(cff + privOff, privSize, privOps);
        auto subIt = privOps.find(19);
        if (subIt != privOps.end() && !subIt->second.empty()) {
            uint32_t subRel = static_cast<uint32_t>(std::max(0.0f, subIt->second.back()));
            uint32_t subOff = privOff + subRel;
            uint32_t nextP = 0;
            if (subOff < cffLen) {
                readCffIndex(cff, cffLen, subOff, outSubrs, nextP);
            }
        }
    };

    auto privIt = topOps.find(18);
    if (privIt != topOps.end() && privIt->second.size() >= 2) {
        uint32_t pSize = static_cast<uint32_t>(std::max(0.0f, privIt->second[0]));
        uint32_t pOff  = static_cast<uint32_t>(std::max(0.0f, privIt->second[1]));
        parsePrivateSubrs(pSize, pOff, m_cffLocalSubrs);
    }

    // CID-keyed FDArray (1236) and FDSelect (1237)
    auto fdaIt = topOps.find(1236);
    if (fdaIt != topOps.end() && !fdaIt->second.empty()) {
        uint32_t fdaOff = static_cast<uint32_t>(std::max(0.0f, fdaIt->second.back()));
        CffIndex fdIdx;
        if (fdaOff > 0 && fdaOff < cffLen && readCffIndex(cff, cffLen, fdaOff, fdIdx, dummyNext)) {
            m_cffFdSubrs.resize(fdIdx.count());
            for (size_t fi = 0; fi < fdIdx.count(); ++fi) {
                uint32_t fStart = fdIdx.dataStart + fdIdx.offsets[fi];
                uint32_t fLen = fdIdx.offsets[fi + 1] - fdIdx.offsets[fi];
                std::unordered_map<int, std::vector<float>> fdOps;
                parseCffDict(cff + fStart, fLen, fdOps);
                auto fpIt = fdOps.find(18);
                if (fpIt != fdOps.end() && fpIt->second.size() >= 2) {
                    parsePrivateSubrs(
                        static_cast<uint32_t>(std::max(0.0f, fpIt->second[0])),
                        static_cast<uint32_t>(std::max(0.0f, fpIt->second[1])),
                        m_cffFdSubrs[fi]);
                }
            }
        }
    }
    auto fdsIt = topOps.find(1237);
    if (fdsIt != topOps.end() && !fdsIt->second.empty()) {
        uint32_t fdsOff = static_cast<uint32_t>(std::max(0.0f, fdsIt->second.back()));
        if (fdsOff > 0 && fdsOff + 1 < cffLen) {
            m_cffGlyphFd.assign(m_numGlyphs, 0);
            uint8_t fmt = cff[fdsOff];
            if (fmt == 0 && fdsOff + 1 + static_cast<uint32_t>(m_numGlyphs) <= cffLen) {
                for (int g = 0; g < m_numGlyphs; ++g) {
                    m_cffGlyphFd[g] = cff[fdsOff + 1 + g];
                }
            } else if (fmt == 3 && fdsOff + 3 <= cffLen) {
                uint16_t nRanges = readU16BE(cff + fdsOff + 1);
                uint32_t rp = fdsOff + 3;
                for (uint16_t r = 0; r < nRanges && rp + 5 <= cffLen; ++r) {
                    uint16_t firstG = readU16BE(cff + rp);
                    uint8_t fdIdx = cff[rp + 2];
                    uint16_t nextG = readU16BE(cff + rp + 3);
                    for (uint16_t g = firstG; g < nextG && g < static_cast<uint16_t>(m_numGlyphs); ++g) {
                        m_cffGlyphFd[g] = fdIdx;
                    }
                    rp += 3;
                }
            }
        }
    }

    // Helper to resolve SID (String ID) -> glyph name string
    auto sidToString = [&](uint16_t sid) -> std::string {
        if (sid < 391) return kCffStandardStrings[sid];
        size_t idx = sid - 391;
        if (idx < stringIdx.count()) {
            uint32_t sOff = stringIdx.dataStart + stringIdx.offsets[idx];
            uint32_t sLen = stringIdx.offsets[idx + 1] - stringIdx.offsets[idx];
            if (sOff + sLen <= cffLen) {
                return std::string(reinterpret_cast<const char *>(cff + sOff), sLen);
            }
        }
        return "";
    };

    // Parse Charset (operator 15) to map GID <-> SID/CID <-> Glyph Name <-> Unicode
    std::vector<uint16_t> gidToSid(m_numGlyphs, 0);
    std::unordered_map<uint16_t, int> sidToGid;
    uint32_t charsetOff = 0;
    auto chsIt = topOps.find(15);
    if (chsIt != topOps.end() && !chsIt->second.empty()) {
        charsetOff = static_cast<uint32_t>(std::max(0.0f, chsIt->second.back()));
    }
    if (charsetOff <= 2) {
        // Predefined charsets: 0 = ISOAdobe (SID 1..228), 1 = Expert, 2 = ExpertSubset
        for (int g = 1; g < m_numGlyphs && g <= 228; ++g) {
            gidToSid[g] = static_cast<uint16_t>(g);
        }
    } else if (charsetOff < cffLen) {
        uint8_t fmt = cff[charsetOff];
        uint32_t cp = charsetOff + 1;
        int gid = 1;
        if (fmt == 0) {
            while (gid < m_numGlyphs && cp + 2 <= cffLen) {
                gidToSid[gid++] = readU16BE(cff + cp);
                cp += 2;
            }
        } else if (fmt == 1) {
            while (gid < m_numGlyphs && cp + 3 <= cffLen) {
                uint16_t firstSid = readU16BE(cff + cp);
                uint8_t nLeft = cff[cp + 2];
                cp += 3;
                for (uint32_t k = 0; k <= nLeft && gid < m_numGlyphs; ++k) {
                    gidToSid[gid++] = static_cast<uint16_t>(firstSid + k);
                }
            }
        } else if (fmt == 2) {
            while (gid < m_numGlyphs && cp + 4 <= cffLen) {
                uint16_t firstSid = readU16BE(cff + cp);
                uint16_t nLeft = readU16BE(cff + cp + 2);
                cp += 4;
                for (uint32_t k = 0; k <= nLeft && gid < m_numGlyphs; ++k) {
                    gidToSid[gid++] = static_cast<uint16_t>(firstSid + k);
                }
            }
        }
    }

    for (int g = 1; g < m_numGlyphs; ++g) {
        uint16_t sid = gidToSid[g];
        if (sid > 0 && sidToGid.find(sid) == sidToGid.end()) {
            sidToGid[sid] = g;
        }
        if (m_cffIsCid) {
            m_cffCodeToGid[sid] = g;
        } else if (sid > 0) {
            std::string gName = sidToString(sid);
            if (!gName.empty()) {
                if (m_cffNameToGid.find(gName) == m_cffNameToGid.end()) {
                    m_cffNameToGid[gName] = g;
                }
                uint32_t u = pdfGlyphNameToUnicode(gName);
                if (u > 0 && m_cffUnicodeToGid.find(u) == m_cffUnicodeToGid.end()) {
                    m_cffUnicodeToGid[u] = g;
                }
            }
        }
    }

    // Parse CFF Encoding table (operator 16) if present
    uint32_t encOff = 0;
    auto encIt = topOps.find(16);
    if (encIt != topOps.end() && !encIt->second.empty()) {
        encOff = static_cast<uint32_t>(std::max(0.0f, encIt->second.back()));
    }
    if (!m_cffIsCid && encOff > 1 && encOff < cffLen) {
        uint8_t fmt = cff[encOff];
        uint32_t ep = encOff + 1;
        uint8_t baseFmt = fmt & 0x7F;
        if (baseFmt == 0 && ep < cffLen) {
            uint8_t nCodes = cff[ep++];
            for (uint8_t i = 0; i < nCodes && ep < cffLen && (i + 1) < m_numGlyphs; ++i) {
                uint8_t code = cff[ep++];
                m_cffCodeToGid[code] = i + 1;
            }
        } else if (baseFmt == 1 && ep < cffLen) {
            uint8_t nRanges = cff[ep++];
            int gid = 1;
            for (uint8_t i = 0; i < nRanges && ep + 2 <= cffLen && gid < m_numGlyphs; ++i) {
                uint8_t firstCode = cff[ep];
                uint8_t nLeft = cff[ep + 1];
                ep += 2;
                for (uint16_t k = 0; k <= nLeft && gid < m_numGlyphs; ++k) {
                    m_cffCodeToGid[static_cast<uint16_t>((firstCode + k) & 0xFF)] = gid++;
                }
            }
        }
        // Supplemental encoding entries (high bit 0x80)
        if ((fmt & 0x80) != 0 && ep < cffLen) {
            uint8_t nSups = cff[ep++];
            for (uint8_t i = 0; i < nSups && ep + 3 <= cffLen; ++i) {
                uint8_t code = cff[ep];
                uint16_t sid = readU16BE(cff + ep + 1);
                ep += 3;
                auto sIt = sidToGid.find(sid);
                if (sIt != sidToGid.end()) {
                    m_cffCodeToGid[code] = sIt->second;
                }
            }
        }
    }

    m_isCff = true;
    m_valid = true;
    return true;
}

bool PdfTrueTypeFont::load(std::vector<uint8_t> &&fontData) {
    m_data = std::move(fontData);
    m_valid = false;
    m_isCff = false;
    m_cffIsCid = false;
    m_unitsPerEm = 1000;
    m_numGlyphs = 0;
    m_numHMetrics = 0;
    m_cmapOffset = m_cmapLen = 0;
    m_locaOffset = m_locaLen = 0;
    m_glyfOffset = m_glyfLen = 0;
    m_hmtxOffset = m_hmtxLen = 0;
    m_cffCharStrings = {};
    m_cffGlobalSubrs = {};
    m_cffLocalSubrs = {};
    m_cffFdSubrs.clear();
    m_cffGlyphFd.clear();
    m_cffUnicodeToGid.clear();
    m_cffNameToGid.clear();
    m_cffCodeToGid.clear();

    if (m_data.size() < 16) return false;

    // Case 1: Raw CFF /Type1C stream (starts with CFF header major=1, minor=0, hdrSize>=4, offSize=1..4)
    if (m_data[0] == 1 && m_data[1] == 0 && m_data[2] >= 4 && m_data[3] >= 1 && m_data[3] <= 4) {
        return parseCff(0, static_cast<uint32_t>(m_data.size()));
    }

    if (m_data.size() < 32) return false;

    uint16_t numTables = readU16BE(m_data.data() + 4);
    if (numTables == 0 || numTables > 128 || 12 + static_cast<size_t>(numTables) * 16 > m_data.size()) {
        return false;
    }

    uint32_t headOff = 0, headLen = 0;
    uint32_t maxpOff = 0, maxpLen = 0;
    uint32_t hheaOff = 0, hheaLen = 0;
    uint32_t cffOff = 0, cffLen = 0;

    for (uint16_t i = 0; i < numTables; ++i) {
        const uint8_t *rec = m_data.data() + 12 + static_cast<size_t>(i) * 16;
        uint32_t off = readU32BE(rec + 8);
        uint32_t len = readU32BE(rec + 12);
        if (static_cast<uint64_t>(off) + len > m_data.size()) continue;

        if (memcmp(rec, "head", 4) == 0) { headOff = off; headLen = len; }
        else if (memcmp(rec, "maxp", 4) == 0) { maxpOff = off; maxpLen = len; }
        else if (memcmp(rec, "hhea", 4) == 0) { hheaOff = off; hheaLen = len; }
        else if (memcmp(rec, "hmtx", 4) == 0) { m_hmtxOffset = off; m_hmtxLen = len; }
        else if (memcmp(rec, "loca", 4) == 0) { m_locaOffset = off; m_locaLen = len; }
        else if (memcmp(rec, "glyf", 4) == 0) { m_glyfOffset = off; m_glyfLen = len; }
        else if (memcmp(rec, "cmap", 4) == 0) { m_cmapOffset = off; m_cmapLen = len; }
        else if (memcmp(rec, "CFF ", 4) == 0) { cffOff = off; cffLen = len; }
    }

    if (headOff > 0 && headLen >= 54) {
        m_unitsPerEm = readU16BE(m_data.data() + headOff + 18);
        if (m_unitsPerEm <= 0) m_unitsPerEm = 1000;
        m_indexToLocFormat = readS16BE(m_data.data() + headOff + 50);
    }
    if (maxpOff > 0 && maxpLen >= 6) {
        m_numGlyphs = readU16BE(m_data.data() + maxpOff + 4);
    }
    if (hheaOff > 0 && hheaLen >= 36) {
        m_numHMetrics = readU16BE(m_data.data() + hheaOff + 34);
    }

    // Case 2: OpenType CFF ('OTTO') font with 'CFF ' table
    if (cffOff > 0 && cffLen >= 16) {
        int savedUpem = m_unitsPerEm;
        if (parseCff(cffOff, cffLen)) {
            if (headOff > 0 && savedUpem > 0) m_unitsPerEm = savedUpem;
            return true;
        }
    }

    // Case 3: Standard TrueType ('glyf' + 'loca') font
    if (headOff == 0 || headLen < 54 || m_locaOffset == 0 || m_glyfOffset == 0) {
        return false;
    }

    if (m_numGlyphs <= 0) {
        m_numGlyphs = (m_indexToLocFormat == 0)
            ? (static_cast<int>(m_locaLen) / 2 - 1)
            : (static_cast<int>(m_locaLen) / 4 - 1);
    }

    m_valid = (m_numGlyphs > 0 && m_glyfLen > 0);
    return m_valid;
}

int PdfTrueTypeFont::findGlyphByName(const std::string &glyphName) const {
    if (!m_valid || glyphName.empty()) return 0;
    std::string clean = (glyphName[0] == '/') ? glyphName.substr(1) : glyphName;
    auto it = m_cffNameToGid.find(clean);
    if (it != m_cffNameToGid.end() && it->second > 0 && it->second < m_numGlyphs) {
        return it->second;
    }
    uint32_t u = pdfGlyphNameToUnicode(clean);
    if (u > 0) {
        return findGlyphIndex(u);
    }
    return 0;
}

int PdfTrueTypeFont::findGlyphByCharCode(uint16_t code) const {
    if (!m_valid) return 0;
    auto it = m_cffCodeToGid.find(code);
    if (it != m_cffCodeToGid.end() && it->second > 0 && it->second < m_numGlyphs) {
        return it->second;
    }
    return 0;
}

int PdfTrueTypeFont::findGlyphIndex(uint32_t unicodeCp) const {
    if (!m_valid) return 0;
    if (!m_cffUnicodeToGid.empty()) {
        auto it = m_cffUnicodeToGid.find(unicodeCp);
        if (it != m_cffUnicodeToGid.end() && it->second > 0 && it->second < m_numGlyphs) {
            return it->second;
        }
    }
    if (m_cmapOffset == 0 || m_cmapLen < 4) return 0;
    const uint8_t *cmap = m_data.data() + m_cmapOffset;
    uint16_t numTables = readU16BE(cmap + 2);
    if (4 + static_cast<size_t>(numTables) * 8 > m_cmapLen) return 0;

    auto lookupSubtable = [&](uint32_t subOff) -> int {
        if (subOff + 6 > m_cmapLen) return 0;
        const uint8_t *sub = cmap + subOff;
        uint16_t format = readU16BE(sub);

        if (format == 0) {
            if (unicodeCp < 256 && subOff + 6 + unicodeCp < m_cmapLen) {
                return sub[6 + unicodeCp];
            }
        } else if (format == 4 && unicodeCp <= 0xFFFF) {
            if (subOff + 14 > m_cmapLen) return 0;
            uint16_t segCountX2 = readU16BE(sub + 6);
            uint16_t segCount = segCountX2 / 2;
            uint32_t endCodeOff = 14;
            uint32_t startCodeOff = endCodeOff + segCountX2 + 2;
            uint32_t idDeltaOff = startCodeOff + segCountX2;
            uint32_t idRangeOff = idDeltaOff + segCountX2;
            if (subOff + idRangeOff + segCountX2 > m_cmapLen) return 0;

            for (uint16_t i = 0; i < segCount; ++i) {
                uint16_t endCode = readU16BE(sub + endCodeOff + i * 2);
                if (unicodeCp > endCode) continue;
                uint16_t startCode = readU16BE(sub + startCodeOff + i * 2);
                if (unicodeCp < startCode) return 0;

                int16_t idDelta = readS16BE(sub + idDeltaOff + i * 2);
                uint16_t idRangeOffset = readU16BE(sub + idRangeOff + i * 2);
                if (idRangeOffset == 0) {
                    return static_cast<int>((unicodeCp + idDelta) & 0xFFFF);
                }
                uint32_t glyphIdxOff = idRangeOff + i * 2 + idRangeOffset + 2 * (unicodeCp - startCode);
                if (subOff + glyphIdxOff + 2 > m_cmapLen) return 0;
                uint16_t gid = readU16BE(sub + glyphIdxOff);
                if (gid == 0) return 0;
                return static_cast<int>((gid + idDelta) & 0xFFFF);
            }
        } else if (format == 6 && unicodeCp <= 0xFFFF) {
            if (subOff + 10 > m_cmapLen) return 0;
            uint16_t firstCode = readU16BE(sub + 6);
            uint16_t entryCount = readU16BE(sub + 8);
            if (unicodeCp >= firstCode && unicodeCp < static_cast<uint32_t>(firstCode + entryCount)) {
                uint32_t idxOff = 10 + (unicodeCp - firstCode) * 2;
                if (subOff + idxOff + 2 <= m_cmapLen) {
                    return readU16BE(sub + idxOff);
                }
            }
        } else if (format == 12) {
            if (subOff + 16 > m_cmapLen) return 0;
            uint32_t numGroups = readU32BE(sub + 12);
            for (uint32_t i = 0; i < numGroups; ++i) {
                uint32_t gOff = 16 + i * 12;
                if (subOff + gOff + 12 > m_cmapLen) break;
                uint32_t startCode = readU32BE(sub + gOff);
                uint32_t endCode = readU32BE(sub + gOff + 4);
                uint32_t startGid = readU32BE(sub + gOff + 8);
                if (unicodeCp >= startCode && unicodeCp <= endCode) {
                    return static_cast<int>(startGid + (unicodeCp - startCode));
                }
            }
        }
        return 0;
    };

    // First pass: try Unicode / Windows BMP / UCS-4 tables (Platform 0 or Platform 3)
    for (uint16_t i = 0; i < numTables; ++i) {
        const uint8_t *rec = cmap + 4 + static_cast<size_t>(i) * 8;
        uint16_t platformId = readU16BE(rec);
        uint32_t subOff = readU32BE(rec + 4);
        if (platformId == 0 || platformId == 3) {
            int gid = lookupSubtable(subOff);
            if (gid > 0 && gid < m_numGlyphs) return gid;
        }
    }

    // Second pass: try any subtable (e.g. Platform 1 Mac / Symbol)
    for (uint16_t i = 0; i < numTables; ++i) {
        const uint8_t *rec = cmap + 4 + static_cast<size_t>(i) * 8;
        uint32_t subOff = readU32BE(rec + 4);
        int gid = lookupSubtable(subOff);
        if (gid > 0 && gid < m_numGlyphs) return gid;
        if (unicodeCp < 256) {
            // Microsoft Symbol cmap often maps 0x00..0xFF at 0xF000 + code
            gid = lookupSubtable(subOff);
            if (gid == 0) {
                uint32_t symCp = 0xF000 + unicodeCp;
                std::swap(unicodeCp, symCp);
                gid = lookupSubtable(subOff);
                std::swap(unicodeCp, symCp);
                if (gid > 0 && gid < m_numGlyphs) return gid;
            }
        }
    }

    return 0;
}

int PdfTrueTypeFont::getGlyphWidth1000(int gid) const {
    if (!m_valid || m_hmtxOffset == 0 || m_numHMetrics <= 0 || gid < 0) return 0;
    int metricIdx = std::min(gid, m_numHMetrics - 1);
    uint32_t off = m_hmtxOffset + static_cast<uint32_t>(metricIdx) * 4;
    if (off + 2 > m_data.size()) return 0;
    uint16_t adv = readU16BE(m_data.data() + off);
    return (static_cast<int>(adv) * 1000 + m_unitsPerEm / 2) / m_unitsPerEm;
}

bool PdfTrueTypeFont::getGlyphLocation(int gid, uint32_t &outOffset, uint32_t &outLen) const {
    if (!m_valid || gid < 0 || gid >= m_numGlyphs) return false;
    uint32_t off0 = 0, off1 = 0;
    if (m_indexToLocFormat == 0) {
        uint32_t lOff = m_locaOffset + static_cast<uint32_t>(gid) * 2;
        if (lOff + 4 > m_locaOffset + m_locaLen || lOff + 4 > m_data.size()) return false;
        off0 = static_cast<uint32_t>(readU16BE(m_data.data() + lOff)) * 2;
        off1 = static_cast<uint32_t>(readU16BE(m_data.data() + lOff + 2)) * 2;
    } else {
        uint32_t lOff = m_locaOffset + static_cast<uint32_t>(gid) * 4;
        if (lOff + 8 > m_locaOffset + m_locaLen || lOff + 8 > m_data.size()) return false;
        off0 = readU32BE(m_data.data() + lOff);
        off1 = readU32BE(m_data.data() + lOff + 4);
    }
    if (off1 < off0 || off1 > m_glyfLen) return false;
    outOffset = m_glyfOffset + off0;
    outLen = off1 - off0;
    return true;
}

bool PdfTrueTypeFont::appendGlyphContours(
    int gid,
    const float matrix[6],
    PdfCanvas &canvas,
    int depth) const
{
    if (depth > 8) return false;
    uint32_t gOff = 0, gLen = 0;
    if (!getGlyphLocation(gid, gOff, gLen)) return false;
    if (gLen == 0) return true; // Whitespace glyph (e.g. space)
    if (gLen < 10 || gOff + gLen > m_data.size()) return false;

    const uint8_t *gPtr = m_data.data() + gOff;
    const uint8_t *gEnd = gPtr + gLen;
    int16_t numContours = readS16BE(gPtr);

    if (numContours == 0) {
        return true;
    }

    if (numContours > 0) {
        // Simple glyph
        if (10 + static_cast<size_t>(numContours) * 2 + 2 > gLen) return false;
        std::vector<uint16_t> endPts(numContours);
        for (int i = 0; i < numContours; ++i) {
            endPts[i] = readU16BE(gPtr + 10 + i * 2);
        }
        int numPts = static_cast<int>(endPts.back()) + 1;
        if (numPts <= 0 || numPts > 4096) return false;

        const uint8_t *p = gPtr + 10 + numContours * 2;
        uint16_t insnLen = readU16BE(p);
        p += 2 + insnLen;
        if (p >= gEnd) return false;

        std::vector<uint8_t> flags(numPts);
        for (int i = 0; i < numPts;) {
            if (p >= gEnd) return false;
            uint8_t f = *p++;
            flags[i++] = f;
            if (f & 0x08) { // REPEAT_FLAG
                if (p >= gEnd) return false;
                uint8_t rep = *p++;
                while (rep-- > 0 && i < numPts) {
                    flags[i++] = f;
                }
            }
        }

        std::vector<int> xCoords(numPts, 0);
        int curX = 0;
        for (int i = 0; i < numPts; ++i) {
            uint8_t f = flags[i];
            if (f & 0x02) { // X_SHORT_VECTOR
                if (p >= gEnd) return false;
                int dx = *p++;
                curX += (f & 0x10) ? dx : -dx;
            } else if (!(f & 0x10)) {
                if (p + 2 > gEnd) return false;
                curX += readS16BE(p);
                p += 2;
            }
            xCoords[i] = curX;
        }

        std::vector<int> yCoords(numPts, 0);
        int curY = 0;
        for (int i = 0; i < numPts; ++i) {
            uint8_t f = flags[i];
            if (f & 0x04) { // Y_SHORT_VECTOR
                if (p >= gEnd) return false;
                int dy = *p++;
                curY += (f & 0x20) ? dy : -dy;
            } else if (!(f & 0x20)) {
                if (p + 2 > gEnd) return false;
                curY += readS16BE(p);
                p += 2;
            }
            yCoords[i] = curY;
        }

        auto xformPt = [&](int fx, int fy) -> PdfCanvas::Point {
            return {
                matrix[0] * static_cast<float>(fx) + matrix[2] * static_cast<float>(fy) + matrix[4],
                matrix[1] * static_cast<float>(fx) + matrix[3] * static_cast<float>(fy) + matrix[5]
            };
        };

        int startIdx = 0;
        for (int c = 0; c < numContours; ++c) {
            int endIdx = static_cast<int>(endPts[c]);
            int count = endIdx - startIdx + 1;
            if (count >= 2 && endIdx < numPts) {
                struct CPoint {
                    PdfCanvas::Point pt;
                    bool onCurve;
                };
                std::vector<CPoint> cpts(count);
                for (int i = 0; i < count; ++i) {
                    int idx = startIdx + i;
                    cpts[i] = { xformPt(xCoords[idx], yCoords[idx]), (flags[idx] & 0x01) != 0 };
                }

                // Determine starting on-curve point
                PdfCanvas::Point startPt;
                int firstStep = 0;
                if (cpts[0].onCurve) {
                    startPt = cpts[0].pt;
                    firstStep = 1;
                } else if (cpts.back().onCurve) {
                    startPt = cpts.back().pt;
                    firstStep = 0;
                } else {
                    startPt = {
                        0.5f * (cpts.back().pt.x + cpts[0].pt.x),
                        0.5f * (cpts.back().pt.y + cpts[0].pt.y)
                    };
                    firstStep = 0;
                }

                canvas.moveTo(startPt.x, startPt.y);
                int i = firstStep;
                while (i < count) {
                    if (cpts[i].onCurve) {
                        canvas.lineTo(cpts[i].pt.x, cpts[i].pt.y);
                        i++;
                    } else {
                        const CPoint &ctrl = cpts[i];
                        const CPoint &next = cpts[(i + 1) % count];
                        if (next.onCurve) {
                            canvas.quadTo(ctrl.pt.x, ctrl.pt.y, next.pt.x, next.pt.y);
                            if (i + 1 < count) {
                                i += 2;
                            } else {
                                break;
                            }
                        } else {
                            PdfCanvas::Point mid = {
                                0.5f * (ctrl.pt.x + next.pt.x),
                                0.5f * (ctrl.pt.y + next.pt.y)
                            };
                            canvas.quadTo(ctrl.pt.x, ctrl.pt.y, mid.x, mid.y);
                            i++;
                        }
                    }
                }
                canvas.closePath();
            }
            startIdx = endIdx + 1;
        }
        return true;
    }

    // Compound glyph (numContours < 0)
    const uint8_t *p = gPtr + 10;
    uint16_t compFlags = 0;
    do {
        if (p + 4 > gEnd) break;
        compFlags = readU16BE(p);
        uint16_t subGid = readU16BE(p + 2);
        p += 4;

        float dx = 0.0f, dy = 0.0f;
        if (compFlags & 0x0001) { // ARG_1_AND_2_ARE_WORDS
            if (p + 4 > gEnd) break;
            if (compFlags & 0x0002) { // ARGS_ARE_XY_VALUES
                dx = static_cast<float>(readS16BE(p));
                dy = static_cast<float>(readS16BE(p + 2));
            }
            p += 4;
        } else {
            if (p + 2 > gEnd) break;
            if (compFlags & 0x0002) {
                dx = static_cast<float>(static_cast<int8_t>(p[0]));
                dy = static_cast<float>(static_cast<int8_t>(p[1]));
            }
            p += 2;
        }

        float a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f;
        if (compFlags & 0x0008) { // WE_HAVE_A_SCALE
            if (p + 2 > gEnd) break;
            a = d = static_cast<float>(readS16BE(p)) / 16384.0f;
            p += 2;
        } else if (compFlags & 0x0040) { // WE_HAVE_AN_X_AND_Y_SCALE
            if (p + 4 > gEnd) break;
            a = static_cast<float>(readS16BE(p)) / 16384.0f;
            d = static_cast<float>(readS16BE(p + 2)) / 16384.0f;
            p += 4;
        } else if (compFlags & 0x0080) { // WE_HAVE_A_TWO_BY_TWO
            if (p + 8 > gEnd) break;
            a = static_cast<float>(readS16BE(p)) / 16384.0f;
            b = static_cast<float>(readS16BE(p + 2)) / 16384.0f;
            c = static_cast<float>(readS16BE(p + 4)) / 16384.0f;
            d = static_cast<float>(readS16BE(p + 6)) / 16384.0f;
            p += 8;
        }

        float subMat[6] = {
            a * matrix[0] + b * matrix[2],
            a * matrix[1] + b * matrix[3],
            c * matrix[0] + d * matrix[2],
            c * matrix[1] + d * matrix[3],
            dx * matrix[0] + dy * matrix[2] + matrix[4],
            dx * matrix[1] + dy * matrix[3] + matrix[5]
        };
        appendGlyphContours(subGid, subMat, canvas, depth + 1);
    } while (compFlags & 0x0020); // MORE_COMPONENTS

    return true;
}

bool PdfTrueTypeFont::appendCffGlyphContours(
    int gid,
    const float matrix[6],
    PdfCanvas &canvas) const
{
    if (!m_valid || !m_isCff || gid < 0 || gid >= static_cast<int>(m_cffCharStrings.count())) {
        return false;
    }

    const uint8_t *cff = m_data.data() + m_cffOffset;
    uint32_t csOff = m_cffCharStrings.dataStart + m_cffCharStrings.offsets[gid];
    uint32_t csLen = m_cffCharStrings.offsets[gid + 1] - m_cffCharStrings.offsets[gid];
    if (csLen == 0 || csOff + csLen > m_cffLen) return false;

    const CffIndex *localSubrs = &m_cffLocalSubrs;
    if (gid < static_cast<int>(m_cffGlyphFd.size())) {
        uint8_t fd = m_cffGlyphFd[gid];
        if (fd < m_cffFdSubrs.size() && m_cffFdSubrs[fd].count() > 0) {
            localSubrs = &m_cffFdSubrs[fd];
        }
    }
    const int localBias = computeCffSubrBias(localSubrs->count());
    const int globalBias = computeCffSubrBias(m_cffGlobalSubrs.count());

    float stack[64];
    int sp = 0;
    float transient[32] = { 0.0f };
    float curX = 0.0f;
    float curY = 0.0f;
    bool openSubpath = false;
    bool seenWidth = false;
    int numHints = 0;
    bool ended = false;

    auto pushVal = [&](float v) {
        if (sp < 64) {
            stack[sp++] = v;
        } else {
            memmove(stack, stack + 1, 63 * sizeof(float));
            stack[63] = v;
        }
    };

    auto popVal = [&]() -> float {
        return (sp > 0) ? stack[--sp] : 0.0f;
    };

    auto xformPt = [&](float fx, float fy) -> PdfCanvas::Point {
        return {
            matrix[0] * fx + matrix[2] * fy + matrix[4],
            matrix[1] * fx + matrix[3] * fy + matrix[5]
        };
    };

    auto emitMoveTo = [&](float fx, float fy) {
        if (openSubpath) {
            canvas.closePath();
        }
        PdfCanvas::Point p = xformPt(fx, fy);
        canvas.moveTo(p.x, p.y);
        openSubpath = true;
    };

    auto emitLineTo = [&](float fx, float fy) {
        PdfCanvas::Point p = xformPt(fx, fy);
        canvas.lineTo(p.x, p.y);
    };

    auto emitCubicTo = [&](float x1, float y1, float x2, float y2, float x3, float y3) {
        PdfCanvas::Point p1 = xformPt(x1, y1);
        PdfCanvas::Point p2 = xformPt(x2, y2);
        PdfCanvas::Point p3 = xformPt(x3, y3);
        canvas.cubicTo(p1.x, p1.y, p2.x, p2.y, p3.x, p3.y);
    };

    auto execCharString = [&](auto &self, const uint8_t *code, uint32_t len, int depth) -> void {
        if (depth > 12 || ended) return;
        uint32_t ip = 0;
        while (ip < len && !ended) {
            uint8_t b0 = code[ip++];
            if (b0 == 28) {
                if (ip + 2 > len) break;
                int16_t v = static_cast<int16_t>((code[ip] << 8) | code[ip + 1]);
                ip += 2;
                pushVal(static_cast<float>(v));
            } else if (b0 >= 32 && b0 <= 246) {
                pushVal(static_cast<float>(static_cast<int>(b0) - 139));
            } else if (b0 >= 247 && b0 <= 250) {
                if (ip >= len) break;
                uint8_t b1 = code[ip++];
                pushVal(static_cast<float>((static_cast<int>(b0) - 247) * 256 + static_cast<int>(b1) + 108));
            } else if (b0 >= 251 && b0 <= 254) {
                if (ip >= len) break;
                uint8_t b1 = code[ip++];
                pushVal(static_cast<float>(-(static_cast<int>(b0) - 251) * 256 - static_cast<int>(b1) - 108));
            } else if (b0 == 255) {
                if (ip + 4 > len) break;
                int32_t raw = static_cast<int32_t>(
                    (static_cast<uint32_t>(code[ip]) << 24) |
                    (static_cast<uint32_t>(code[ip + 1]) << 16) |
                    (static_cast<uint32_t>(code[ip + 2]) << 8) |
                    static_cast<uint32_t>(code[ip + 3]));
                ip += 4;
                pushVal(static_cast<float>(raw) / 65536.0f);
            } else {
                // Type 2 Operator
                switch (b0) {
                    case 1:  // hstem
                    case 3:  // vstem
                    case 18: // hstemhm
                    case 23: // vstemhm
                        if (!seenWidth && (sp & 1) == 1) seenWidth = true;
                        numHints += sp / 2;
                        sp = 0;
                        break;

                    case 19: // hintmask
                    case 20: // cntrmask
                        if (sp > 0) {
                            if (!seenWidth && (sp & 1) == 1) seenWidth = true;
                            numHints += sp / 2;
                            sp = 0;
                        }
                        ip += static_cast<uint32_t>((numHints + 7) / 8);
                        break;

                    case 21: // rmoveto
                        if (sp >= 2) {
                            if (!seenWidth && sp > 2) seenWidth = true;
                            seenWidth = true;
                            curX += stack[sp - 2];
                            curY += stack[sp - 1];
                            emitMoveTo(curX, curY);
                        }
                        sp = 0;
                        break;

                    case 22: // hmoveto
                        if (sp >= 1) {
                            if (!seenWidth && sp > 1) seenWidth = true;
                            seenWidth = true;
                            curX += stack[sp - 1];
                            emitMoveTo(curX, curY);
                        }
                        sp = 0;
                        break;

                    case 4: // vmoveto
                        if (sp >= 1) {
                            if (!seenWidth && sp > 1) seenWidth = true;
                            seenWidth = true;
                            curY += stack[sp - 1];
                            emitMoveTo(curX, curY);
                        }
                        sp = 0;
                        break;

                    case 5: // rlineto
                        for (int i = 0; i + 1 < sp; i += 2) {
                            curX += stack[i];
                            curY += stack[i + 1];
                            emitLineTo(curX, curY);
                        }
                        sp = 0;
                        break;

                    case 6: // hlineto
                        for (int i = 0; i < sp; ++i) {
                            if ((i & 1) == 0) curX += stack[i];
                            else curY += stack[i];
                            emitLineTo(curX, curY);
                        }
                        sp = 0;
                        break;

                    case 7: // vlineto
                        for (int i = 0; i < sp; ++i) {
                            if ((i & 1) == 0) curY += stack[i];
                            else curX += stack[i];
                            emitLineTo(curX, curY);
                        }
                        sp = 0;
                        break;

                    case 8: // rrcurveto
                        for (int i = 0; i + 5 < sp; i += 6) {
                            float x1 = curX + stack[i];
                            float y1 = curY + stack[i + 1];
                            float x2 = x1 + stack[i + 2];
                            float y2 = y1 + stack[i + 3];
                            curX = x2 + stack[i + 4];
                            curY = y2 + stack[i + 5];
                            emitCubicTo(x1, y1, x2, y2, curX, curY);
                        }
                        sp = 0;
                        break;

                    case 24: { // rcurveline
                        int i = 0;
                        for (; i + 7 < sp; i += 6) {
                            float x1 = curX + stack[i];
                            float y1 = curY + stack[i + 1];
                            float x2 = x1 + stack[i + 2];
                            float y2 = y1 + stack[i + 3];
                            curX = x2 + stack[i + 4];
                            curY = y2 + stack[i + 5];
                            emitCubicTo(x1, y1, x2, y2, curX, curY);
                        }
                        if (i + 1 < sp) {
                            curX += stack[i];
                            curY += stack[i + 1];
                            emitLineTo(curX, curY);
                        }
                        sp = 0;
                        break;
                    }

                    case 25: { // rlinecurve
                        int i = 0;
                        for (; i + 7 < sp; i += 2) {
                            curX += stack[i];
                            curY += stack[i + 1];
                            emitLineTo(curX, curY);
                        }
                        if (i + 5 < sp) {
                            float x1 = curX + stack[i];
                            float y1 = curY + stack[i + 1];
                            float x2 = x1 + stack[i + 2];
                            float y2 = y1 + stack[i + 3];
                            curX = x2 + stack[i + 4];
                            curY = y2 + stack[i + 5];
                            emitCubicTo(x1, y1, x2, y2, curX, curY);
                        }
                        sp = 0;
                        break;
                    }

                    case 26: { // vvcurveto
                        int i = 0;
                        float dx1 = 0.0f;
                        if ((sp & 1) == 1) {
                            dx1 = stack[i++];
                        }
                        for (; i + 3 < sp; i += 4) {
                            float x1 = curX + dx1;
                            float y1 = curY + stack[i];
                            float x2 = x1 + stack[i + 1];
                            float y2 = y1 + stack[i + 2];
                            curX = x2;
                            curY = y2 + stack[i + 3];
                            emitCubicTo(x1, y1, x2, y2, curX, curY);
                            dx1 = 0.0f;
                        }
                        sp = 0;
                        break;
                    }

                    case 27: { // hhcurveto
                        int i = 0;
                        float dy1 = 0.0f;
                        if ((sp & 1) == 1) {
                            dy1 = stack[i++];
                        }
                        for (; i + 3 < sp; i += 4) {
                            float x1 = curX + stack[i];
                            float y1 = curY + dy1;
                            float x2 = x1 + stack[i + 1];
                            float y2 = y1 + stack[i + 2];
                            curX = x2 + stack[i + 3];
                            curY = y2;
                            emitCubicTo(x1, y1, x2, y2, curX, curY);
                            dy1 = 0.0f;
                        }
                        sp = 0;
                        break;
                    }

                    case 30: // vhcurveto
                    case 31: { // hvcurveto
                        bool vertFirst = (b0 == 30);
                        int i = 0;
                        while (i + 3 < sp) {
                            bool lastExtra = (i + 5 == sp);
                            if (vertFirst) {
                                float x1 = curX;
                                float y1 = curY + stack[i];
                                float x2 = x1 + stack[i + 1];
                                float y2 = y1 + stack[i + 2];
                                curX = x2 + stack[i + 3];
                                curY = y2 + (lastExtra ? stack[i + 4] : 0.0f);
                                emitCubicTo(x1, y1, x2, y2, curX, curY);
                            } else {
                                float x1 = curX + stack[i];
                                float y1 = curY;
                                float x2 = x1 + stack[i + 1];
                                float y2 = y1 + stack[i + 2];
                                curX = x2 + (lastExtra ? stack[i + 4] : 0.0f);
                                curY = y2 + stack[i + 3];
                                emitCubicTo(x1, y1, x2, y2, curX, curY);
                            }
                            i += lastExtra ? 5 : 4;
                            vertFirst = !vertFirst;
                        }
                        sp = 0;
                        break;
                    }

                    case 10: { // callsubr
                        int idx = static_cast<int>(popVal()) + localBias;
                        if (idx >= 0 && static_cast<size_t>(idx) < localSubrs->count()) {
                            uint32_t sOff = localSubrs->dataStart + localSubrs->offsets[idx];
                            uint32_t sLen = localSubrs->offsets[idx + 1] - localSubrs->offsets[idx];
                            if (sOff + sLen <= m_cffLen) {
                                self(self, cff + sOff, sLen, depth + 1);
                            }
                        }
                        break;
                    }

                    case 29: { // callgsubr
                        int idx = static_cast<int>(popVal()) + globalBias;
                        if (idx >= 0 && static_cast<size_t>(idx) < m_cffGlobalSubrs.count()) {
                            uint32_t sOff = m_cffGlobalSubrs.dataStart + m_cffGlobalSubrs.offsets[idx];
                            uint32_t sLen = m_cffGlobalSubrs.offsets[idx + 1] - m_cffGlobalSubrs.offsets[idx];
                            if (sOff + sLen <= m_cffLen) {
                                self(self, cff + sOff, sLen, depth + 1);
                            }
                        }
                        break;
                    }

                    case 11: // return
                        return;

                    case 14: // endchar
                        if (openSubpath) {
                            canvas.closePath();
                            openSubpath = false;
                        }
                        ended = true;
                        sp = 0;
                        return;

                    case 12: { // Two-byte escape operator
                        if (ip >= len) break;
                        uint8_t b1 = code[ip++];
                        switch (b1) {
                            case 34: // hflex
                                if (sp >= 7) {
                                    float x1 = curX + stack[0], y1 = curY;
                                    float x2 = x1 + stack[1],   y2 = y1 + stack[2];
                                    float x3 = x2 + stack[3],   y3 = y2;
                                    float x4 = x3 + stack[4],   y4 = y3;
                                    float x5 = x4 + stack[5],   y5 = curY;
                                    curX = x5 + stack[6];
                                    emitCubicTo(x1, y1, x2, y2, x3, y3);
                                    emitCubicTo(x4, y4, x5, y5, curX, curY);
                                }
                                sp = 0;
                                break;
                            case 35: // flex
                                if (sp >= 12) {
                                    float x1 = curX + stack[0], y1 = curY + stack[1];
                                    float x2 = x1 + stack[2],   y2 = y1 + stack[3];
                                    float x3 = x2 + stack[4],   y3 = y2 + stack[5];
                                    float x4 = x3 + stack[6],   y4 = y3 + stack[7];
                                    float x5 = x4 + stack[8],   y5 = y4 + stack[9];
                                    curX = x5 + stack[10];      curY = y5 + stack[11];
                                    emitCubicTo(x1, y1, x2, y2, x3, y3);
                                    emitCubicTo(x4, y4, x5, y5, curX, curY);
                                }
                                sp = 0;
                                break;
                            case 36: // hflex1
                                if (sp >= 9) {
                                    float x1 = curX + stack[0], y1 = curY + stack[1];
                                    float x2 = x1 + stack[2],   y2 = y1 + stack[3];
                                    float x3 = x2 + stack[4],   y3 = y2;
                                    float x4 = x3 + stack[5],   y4 = y3;
                                    float x5 = x4 + stack[6],   y5 = y4 + stack[7];
                                    curX = x5 + stack[8];
                                    emitCubicTo(x1, y1, x2, y2, x3, y3);
                                    emitCubicTo(x4, y4, x5, y5, curX, curY);
                                }
                                sp = 0;
                                break;
                            case 37: // flex1
                                if (sp >= 11) {
                                    float x1 = curX + stack[0], y1 = curY + stack[1];
                                    float x2 = x1 + stack[2],   y2 = y1 + stack[3];
                                    float x3 = x2 + stack[4],   y3 = y2 + stack[5];
                                    float x4 = x3 + stack[6],   y4 = y3 + stack[7];
                                    float x5 = x4 + stack[8],   y5 = y4 + stack[9];
                                    if (std::fabs(x5 - curX) > std::fabs(y5 - curY)) {
                                        curX = x5 + stack[10];
                                        curY = y5;
                                    } else {
                                        curX = x5;
                                        curY = y5 + stack[10];
                                    }
                                    emitCubicTo(x1, y1, x2, y2, x3, y3);
                                    emitCubicTo(x4, y4, x5, y5, curX, curY);
                                }
                                sp = 0;
                                break;
                            case 9:  if (sp >= 1) stack[sp - 1] = std::fabs(stack[sp - 1]); break;
                            case 10: if (sp >= 2) { float b = popVal(); stack[sp - 1] += b; } break;
                            case 11: if (sp >= 2) { float b = popVal(); stack[sp - 1] -= b; } break;
                            case 12: if (sp >= 2) { float b = popVal(); stack[sp - 1] = (b != 0.0f) ? (stack[sp - 1] / b) : 0.0f; } break;
                            case 14: if (sp >= 1) stack[sp - 1] = -stack[sp - 1]; break;
                            case 15: if (sp >= 2) { float b = popVal(); stack[sp - 1] = (stack[sp - 1] == b) ? 1.0f : 0.0f; } break;
                            case 18: if (sp >= 1) sp--; break;
                            case 20: { float idx = popVal(), val = popVal(); int ti = static_cast<int>(idx); if (ti >= 0 && ti < 32) transient[ti] = val; break; }
                            case 21: { int ti = static_cast<int>(popVal()); pushVal((ti >= 0 && ti < 32) ? transient[ti] : 0.0f); break; }
                            case 22: if (sp >= 4) { float v2 = popVal(), v1 = popVal(), s2 = popVal(), s1 = popVal(); pushVal((v1 <= v2) ? s1 : s2); } break;
                            case 23: pushVal(0.5f); break;
                            case 24: if (sp >= 2) { float b = popVal(); stack[sp - 1] *= b; } break;
                            case 26: if (sp >= 1) stack[sp - 1] = (stack[sp - 1] > 0.0f) ? std::sqrt(stack[sp - 1]) : 0.0f; break;
                            case 27: if (sp >= 1) pushVal(stack[sp - 1]); break;
                            case 28: if (sp >= 2) std::swap(stack[sp - 1], stack[sp - 2]); break;
                            case 29: { int idx = static_cast<int>(popVal()); if (idx < 0) idx = 0; pushVal((sp - 1 - idx >= 0) ? stack[sp - 1 - idx] : 0.0f); break; }
                            default: sp = 0; break;
                        }
                        break;
                    }

                    default:
                        sp = 0;
                        break;
                }
            }
        }
    };

    execCharString(execCharString, cff + csOff, csLen, 0);
    if (openSubpath) {
        canvas.closePath();
    }
    return true;
}

bool PdfTrueTypeFont::hasGlyphOutline(int gid) const {
    if (!m_valid || gid < 0 || gid >= m_numGlyphs) return false;
    if (m_isCff) {
        if (gid >= static_cast<int>(m_cffCharStrings.count())) return false;
        uint32_t csLen = m_cffCharStrings.offsets[gid + 1] - m_cffCharStrings.offsets[gid];
        return csLen > 1;
    }
    uint32_t gOff = 0, gLen = 0;
    if (!getGlyphLocation(gid, gOff, gLen)) return false;
    if (gLen < 10 || gOff + gLen > m_data.size()) return false;
    int16_t numContours = readS16BE(m_data.data() + gOff);
    return numContours != 0;
}

bool PdfTrueTypeFont::renderGlyph(
    int gid,
    const float fontUnitToCanvas[6],
    uint8_t inkGray,
    PdfCanvas &canvas) const
{
    if (!m_valid || gid < 0 || gid >= m_numGlyphs) return false;
    PdfCanvas glyphCanvas(canvas.buffer(), canvas.width(), canvas.height());
    int cx0, cy0, cx1, cy1;
    canvas.getClipRect(cx0, cy0, cx1, cy1);
    glyphCanvas.setClipRect(cx0, cy0, cx1, cy1);

    bool ok = m_isCff
        ? appendCffGlyphContours(gid, fontUnitToCanvas, glyphCanvas)
        : appendGlyphContours(gid, fontUnitToCanvas, glyphCanvas, 0);
    if (!ok || !glyphCanvas.hasPath()) {
        return false;
    }
    float emPx = std::hypot(fontUnitToCanvas[2], fontUnitToCanvas[3]) * static_cast<float>(m_unitsPerEm);
    if (emPx < 1.0f) {
        emPx = std::hypot(fontUnitToCanvas[0], fontUnitToCanvas[1]) * static_cast<float>(m_unitsPerEm);
    }
    float boost = (emPx < 6.2f) ? 2.25f : 1.65f;
    glyphCanvas.fillPath(inkGray, /*evenOdd=*/false, boost);
    return true;
}
