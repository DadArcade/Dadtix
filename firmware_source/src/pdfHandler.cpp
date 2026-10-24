#include "pdfHandler.h"
#include "pdfRasterizer.h"
#include "cbzHandler.h"
#include "renderer.h"
#include "fontHandler.h"
#include "esp_log.h"

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#endif

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_map>
#include <unordered_set>

static const char *TAG = "PDF";

namespace {

static std::string baseNameOf(const std::string &path) {
    size_t pos = path.find_last_of("/\\");
    return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

static std::string stripExtension(const std::string &filename) {
    size_t dot = filename.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return filename;
    return filename.substr(0, dot);
}

static std::string trimPdfString(const std::string &s) {
    size_t start = 0;
    while (start < s.size() && (static_cast<unsigned char>(s[start]) <= 0x20)) {
        start++;
    }
    size_t end = s.size();
    while (end > start && (static_cast<unsigned char>(s[end - 1]) <= 0x20)) {
        end--;
    }
    return s.substr(start, end - start);
}

static inline bool isPdfDelimiter(char c) {
    return c == '(' || c == ')' || c == '<' || c == '>' ||
           c == '[' || c == ']' || c == '{' || c == '}' ||
           c == '/' || c == '%';
}

static inline bool isPdfWhitespace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0' || c == '\f';
}

static void skipPdfWhitespace(const char *&p, const char *end) {
    while (p < end) {
        if (isPdfWhitespace(*p)) {
            ++p;
        } else if (*p == '%') {
            while (p < end && *p != '\r' && *p != '\n') ++p;
        } else {
            break;
        }
    }
}

// Find the closing ">>" of a dictionary starting at dictStart ("<<"),
// properly skipping literal strings (...), hex strings <...>, and nested <<...>>.
static size_t findMatchingDictEnd(const char *str, size_t len, size_t dictStart) {
    if (dictStart + 1 >= len || str[dictStart] != '<' || str[dictStart + 1] != '<') {
        return std::string::npos;
    }
    int depth = 0;
    size_t i = dictStart;
    while (i < len) {
        char c = str[i];
        if (c == '%') {
            while (i < len && str[i] != '\r' && str[i] != '\n') i++;
        } else if (c == '(') {
            int pDepth = 1;
            i++;
            while (i < len && pDepth > 0) {
                if (str[i] == '\\' && i + 1 < len) {
                    i += 2;
                } else if (str[i] == '(') {
                    pDepth++;
                    i++;
                } else if (str[i] == ')') {
                    pDepth--;
                    i++;
                } else {
                    i++;
                }
            }
        } else if (c == '<') {
            if (i + 1 < len && str[i + 1] == '<') {
                depth++;
                i += 2;
            } else {
                // Hex string <...>
                i++;
                while (i < len && str[i] != '>') i++;
                if (i < len) i++;
            }
        } else if (c == '>') {
            if (i + 1 < len && str[i + 1] == '>') {
                depth--;
                i += 2;
                if (depth == 0) {
                    return i;
                }
            } else {
                i++;
            }
        } else {
            i++;
        }
    }
    return std::string::npos;
}

// Extract a top-level /Key Value from a PDF dictionary string "<< ... >>"
static std::string extractDictValue(const std::string &dict, const std::string &key) {
    const char *str = dict.c_str();
    const size_t len = dict.size();
    const size_t keyLen = key.size();

    size_t i = 0;
    while (i < len && isPdfWhitespace(str[i])) i++;

    int dictDepth = 0;
    int arrDepth = 0;

    // If dict doesn't start with "<<", advance to the first "<<" if no '/' key precedes it
    // (e.g. when passed an object header snippet like "obj\r<< ... >>"),
    // otherwise treat outer level as depth 1.
    if (!(i + 1 < len && str[i] == '<' && str[i + 1] == '<')) {
        size_t firstDict = dict.find("<<", i);
        size_t firstSlash = dict.find('/', i);
        if (firstDict != std::string::npos && (firstSlash == std::string::npos || firstDict < firstSlash)) {
            i = firstDict;
        } else {
            dictDepth = 1;
        }
    }

    while (i < len) {
        char c = str[i];
        if (c == '%') {
            while (i < len && str[i] != '\r' && str[i] != '\n') i++;
            continue;
        }
        if (c == '(') {
            int pDepth = 1;
            i++;
            while (i < len && pDepth > 0) {
                if (str[i] == '\\' && i + 1 < len) i += 2;
                else if (str[i] == '(') { pDepth++; i++; }
                else if (str[i] == ')') { pDepth--; i++; }
                else i++;
            }
            continue;
        }
        if (c == '<') {
            if (i + 1 < len && str[i + 1] == '<') {
                dictDepth++;
                i += 2;
            } else {
                i++;
                while (i < len && str[i] != '>') i++;
                if (i < len) i++;
            }
            continue;
        }
        if (c == '>') {
            if (i + 1 < len && str[i + 1] == '>') {
                dictDepth--;
                i += 2;
                if (dictDepth <= 0) break;
            } else {
                i++;
            }
            continue;
        }
        if (c == '[') {
            arrDepth++;
            i++;
            continue;
        }
        if (c == ']') {
            if (arrDepth > 0) arrDepth--;
            i++;
            continue;
        }

        if (c == '/' && dictDepth == 1 && arrDepth == 0) {
            size_t nameStart = i + 1;
            size_t nameEnd = nameStart;
            while (nameEnd < len && !isPdfWhitespace(str[nameEnd]) && !isPdfDelimiter(str[nameEnd])) {
                nameEnd++;
            }
            bool keyMatches = ((nameEnd - nameStart) == keyLen &&
                               memcmp(str + nameStart, key.data(), keyLen) == 0);
            i = nameEnd;

            const char *p = str + i;
            const char *end = str + len;
            skipPdfWhitespace(p, end);
            if (p >= end) return "";

            // Parse the value following this key (so we also skip over values of non-matching keys,
            // ensuring a "/Name" value isn't mistaken for the next key!)
            std::string valStr;
            if (*p == '(') {
                const char *start = p;
                int pDepth = 0;
                while (p < end) {
                    if (*p == '\\' && p + 1 < end) p += 2;
                    else if (*p == '(') { pDepth++; p++; }
                    else if (*p == ')') {
                        pDepth--;
                        p++;
                        if (pDepth == 0) break;
                    } else p++;
                }
                if (keyMatches) return std::string(start, p);
            } else if (*p == '<' && p + 1 < end && *(p + 1) == '<') {
                size_t subStart = static_cast<size_t>(p - str);
                size_t subEnd = findMatchingDictEnd(str, len, subStart);
                if (subEnd == std::string::npos) subEnd = len;
                p = str + subEnd;
                if (keyMatches) return dict.substr(subStart, subEnd - subStart);
            } else if (*p == '<') {
                const char *start = p;
                p++;
                while (p < end && *p != '>') p++;
                if (p < end) p++;
                if (keyMatches) return std::string(start, p);
            } else if (*p == '[') {
                const char *start = p;
                int aDepth = 0;
                while (p < end) {
                    if (*p == '(') {
                        int pDepth = 1;
                        p++;
                        while (p < end && pDepth > 0) {
                            if (*p == '\\' && p + 1 < end) p += 2;
                            else if (*p == '(') { pDepth++; p++; }
                            else if (*p == ')') { pDepth--; p++; }
                            else p++;
                        }
                    } else if (*p == '[') {
                        aDepth++;
                        p++;
                    } else if (*p == ']') {
                        aDepth--;
                        p++;
                        if (aDepth == 0) break;
                    } else {
                        p++;
                    }
                }
                if (keyMatches) return std::string(start, p);
            } else if (*p == '/') {
                // PDF Name value (e.g. /Image, /DCTDecode, /Page, /Pages)
                const char *start = p;
                p++; // skip leading '/'
                while (p < end && !isPdfWhitespace(*p) && !isPdfDelimiter(*p)) {
                    p++;
                }
                if (keyMatches) return std::string(start, p);
            } else {
                // Simple token or indirect reference (e.g. 1234, 10 0 R, true)
                const char *start = p;
                while (p < end && !isPdfWhitespace(*p) && !isPdfDelimiter(*p)) {
                    p++;
                }
                std::string val(start, p);
                const char *p2 = p;
                skipPdfWhitespace(p2, end);
                if (p2 < end && std::isdigit(static_cast<unsigned char>(*p2))) {
                    const char *genStart = p2;
                    while (p2 < end && std::isdigit(static_cast<unsigned char>(*p2))) p2++;
                    const char *rStart = p2;
                    skipPdfWhitespace(rStart, end);
                    if (rStart < end && *rStart == 'R' &&
                        (rStart + 1 >= end || isPdfWhitespace(*(rStart + 1)) || isPdfDelimiter(*(rStart + 1)))) {
                        val += " " + std::string(genStart, p2) + " R";
                        p = rStart + 1;
                    }
                }
                if (keyMatches) return val;
            }
            i = static_cast<size_t>(p - str);
            continue;
        }
        i++;
    }
    return "";
}

static std::string convertPdfBytesToUtf8(const std::string &bytes) {
    if (bytes.size() >= 2 &&
        static_cast<unsigned char>(bytes[0]) == 0xFE &&
        static_cast<unsigned char>(bytes[1]) == 0xFF)
    {
        std::string utf8;
        for (size_t i = 2; i + 1 < bytes.size(); i += 2) {
            uint16_t ch = (static_cast<unsigned char>(bytes[i]) << 8) |
                           static_cast<unsigned char>(bytes[i + 1]);
            if (ch == 0) continue;
            if (ch < 0x80) {
                utf8 += static_cast<char>(ch);
            } else if (ch < 0x800) {
                utf8 += static_cast<char>(0xC0 | (ch >> 6));
                utf8 += static_cast<char>(0x80 | (ch & 0x3F));
            } else {
                utf8 += static_cast<char>(0xE0 | (ch >> 12));
                utf8 += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
                utf8 += static_cast<char>(0x80 | (ch & 0x3F));
            }
        }
        return utf8;
    }
    if (bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF)
    {
        return bytes.substr(3);
    }
    return bytes;
}

// Decode literal or hex PDF string to plain UTF-8
static std::string decodePdfString(const std::string &raw) {
    if (raw.size() >= 2 && raw.front() == '(' && raw.back() == ')') {
        std::string inner = raw.substr(1, raw.size() - 2);
        std::string unescaped;
        unescaped.reserve(inner.size());
        for (size_t i = 0; i < inner.size(); ++i) {
            if (inner[i] == '\\' && i + 1 < inner.size()) {
                char next = inner[++i];
                if (next == 'n') unescaped += '\n';
                else if (next == 'r') unescaped += '\r';
                else if (next == 't') unescaped += '\t';
                else if (next == 'b') unescaped += '\b';
                else if (next == 'f') unescaped += '\f';
                else if (next == '\r') {
                    if (i + 1 < inner.size() && inner[i + 1] == '\n') ++i;
                } else if (next == '\n') {
                    // line continuation
                } else if (next >= '0' && next <= '7') {
                    int oct = next - '0';
                    if (i + 1 < inner.size() && inner[i + 1] >= '0' && inner[i + 1] <= '7') {
                        oct = (oct << 3) + (inner[++i] - '0');
                        if (i + 1 < inner.size() && inner[i + 1] >= '0' && inner[i + 1] <= '7') {
                            oct = (oct << 3) + (inner[++i] - '0');
                        }
                    }
                    unescaped += static_cast<char>(oct);
                } else {
                    unescaped += next;
                }
            } else {
                unescaped += inner[i];
            }
        }
        return convertPdfBytesToUtf8(unescaped);
    } else if (raw.size() >= 2 && raw.front() == '<' && raw.back() == '>') {
        std::string hex = raw.substr(1, raw.size() - 2);
        std::string bytes;
        char hi = 0;
        bool haveHi = false;
        for (size_t i = 0; i < hex.size(); ++i) {
            char c = hex[i];
            if (isPdfWhitespace(c)) continue;
            if (!haveHi) {
                hi = c;
                haveHi = true;
            } else {
                char buf[3] = { hi, c, '\0' };
                bytes += static_cast<char>(strtol(buf, nullptr, 16));
                haveHi = false;
            }
        }
        if (haveHi) {
            char buf[3] = { hi, '0', '\0' };
            bytes += static_cast<char>(strtol(buf, nullptr, 16));
        }
        return convertPdfBytesToUtf8(bytes);
    }
    return raw;
}

// Strictly parse an indirect reference "<id> <gen> R". Returns -1 if not an indirect ref.
static int parseRefObjId(const std::string &refStr) {
    if (refStr.empty()) return -1;
    const char *p = refStr.c_str();
    const char *end = p + refStr.size();
    skipPdfWhitespace(p, end);
    if (p >= end || !std::isdigit(static_cast<unsigned char>(*p))) return -1;
    char *endp = nullptr;
    long id = strtol(p, &endp, 10);
    if (id <= 0 || endp == p) return -1;
    p = endp;
    skipPdfWhitespace(p, end);
    if (p >= end || !std::isdigit(static_cast<unsigned char>(*p))) return -1;
    strtol(p, &endp, 10); // generation number
    if (endp == p) return -1;
    p = endp;
    skipPdfWhitespace(p, end);
    if (p >= end || *p != 'R') return -1;
    p++;
    if (p < end && !isPdfWhitespace(*p) && !isPdfDelimiter(*p)) return -1;
    return static_cast<int>(id);
}

static std::vector<int> parseRefArray(const std::string &arrStr) {
    std::vector<int> ids;
    if (arrStr.empty()) return ids;
    const char *p = arrStr.c_str();
    const char *end = p + arrStr.size();
    while (p < end) {
        skipPdfWhitespace(p, end);
        if (p >= end) break;
        if (*p == '/') {
            // Skip PDF Name token (e.g. /Im0, /Fm1) so trailing digits in the name aren't parsed as object IDs
            p++;
            while (p < end && !isPdfWhitespace(*p) && !isPdfDelimiter(*p)) p++;
        } else if (std::isdigit(static_cast<unsigned char>(*p))) {
            char *endp = nullptr;
            long id = strtol(p, &endp, 10);
            const char *afterId = endp;
            p = afterId;
            skipPdfWhitespace(p, end);
            if (p < end && std::isdigit(static_cast<unsigned char>(*p))) {
                strtol(p, &endp, 10); // gen
                const char *afterGen = endp;
                skipPdfWhitespace(afterGen, end);
                if (afterGen < end && *afterGen == 'R' &&
                    (afterGen + 1 >= end || isPdfWhitespace(*(afterGen + 1)) || isPdfDelimiter(*(afterGen + 1)))) {
                    p = afterGen + 1;
                    if (id > 0) ids.push_back(static_cast<int>(id));
                    continue;
                }
            }
            p = afterId;
        } else {
            p++;
        }
    }
    return ids;
}

static std::vector<int64_t> parseIntArray(const std::string &arrStr) {
    std::vector<int64_t> vals;
    const char *p = arrStr.c_str();
    const char *end = p + arrStr.size();
    while (p < end) {
        if (std::isdigit(static_cast<unsigned char>(*p)) || (*p == '-' && p + 1 < end && std::isdigit(static_cast<unsigned char>(*(p + 1))))) {
            char *endp = nullptr;
            long long v = strtoll(p, &endp, 10);
            vals.push_back(static_cast<int64_t>(v));
            p = endp;
        } else {
            p++;
        }
    }
    return vals;
}

static std::vector<float> parseFloatArray(const std::string &arrStr) {
    std::vector<float> vals;
    const char *p = arrStr.c_str();
    const char *end = p + arrStr.size();
    while (p < end) {
        if (std::isdigit(static_cast<unsigned char>(*p)) ||
            ((*p == '-' || *p == '+' || *p == '.') && p + 1 < end &&
             (std::isdigit(static_cast<unsigned char>(*(p + 1))) || *(p + 1) == '.')))
        {
            char *endp = nullptr;
            float v = strtof(p, &endp);
            if (endp > p) {
                vals.push_back(v);
                p = endp;
                continue;
            }
        }
        p++;
    }
    return vals;
}

static uint8_t *inflatePdfStream(const uint8_t *src, size_t srcLen, size_t &outLen) {
    return inflatePdfDeflateToPsram(src, srcLen, outLen);
}

static inline uint8_t paethPredictor(int a, int b, int c) {
    int p = a + b - c;
    int pa = std::abs(p - a);
    int pb = std::abs(p - b);
    int pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return static_cast<uint8_t>(a);
    if (pb <= pc) return static_cast<uint8_t>(b);
    return static_cast<uint8_t>(c);
}

// Reverses PDF /Predictor in-place (2 = TIFF horizontal differencing, 10..15 = PNG row predictors)
static bool unfilterPdfPredictorInPlace(
    uint8_t *buf, size_t srcLen,
    int predictor, int columns, int colors, int bpc,
    size_t &outLen)
{
    outLen = srcLen;
    if (!buf || srcLen == 0) return false;
    if (predictor <= 1 || columns <= 0) {
        return true;
    }

    int rowBytes = (columns * colors * bpc + 7) / 8;
    int bpp = std::max(1, (colors * bpc + 7) / 8);
    if (rowBytes <= 0) return false;

    if (predictor == 2) {
        // TIFF Predictor 2: no filter byte per row
        size_t rows = srcLen / static_cast<size_t>(rowBytes);
        for (size_t r = 0; r < rows; ++r) {
            uint8_t *row = buf + r * rowBytes;
            for (int c = bpp; c < rowBytes; ++c) {
                row[c] = static_cast<uint8_t>(row[c] + row[c - bpp]);
            }
        }
        outLen = srcLen;
        return true;
    }

    if (predictor >= 10) {
        // PNG Predictors (10..15): 1 filter tag byte at the start of each row.
        // Because r * rowBytes <= r * (rowBytes + 1), we can unfilter in-place!
        size_t stride = static_cast<size_t>(rowBytes) + 1;
        if (srcLen < stride) return false;
        size_t rows = srcLen / stride;

        for (size_t r = 0; r < rows; ++r) {
            const uint8_t *srcRow = buf + r * stride;
            uint8_t filterType = srcRow[0];
            uint8_t *curRow = buf + r * rowBytes;
            const uint8_t *prevRow = (r > 0) ? (buf + (r - 1) * rowBytes) : nullptr;

            for (int c = 0; c < rowBytes; ++c) {
                uint8_t raw = srcRow[1 + c];
                uint8_t left = (c >= bpp) ? curRow[c - bpp] : 0;
                uint8_t up = prevRow ? prevRow[c] : 0;
                uint8_t upLeft = (prevRow && c >= bpp) ? prevRow[c - bpp] : 0;

                switch (filterType) {
                    case 0: curRow[c] = raw; break;
                    case 1: curRow[c] = static_cast<uint8_t>(raw + left); break;
                    case 2: curRow[c] = static_cast<uint8_t>(raw + up); break;
                    case 3: curRow[c] = static_cast<uint8_t>(raw + ((static_cast<int>(left) + static_cast<int>(up)) >> 1)); break;
                    case 4: curRow[c] = static_cast<uint8_t>(raw + paethPredictor(left, up, upLeft)); break;
                    default: curRow[c] = raw; break;
                }
            }
        }
        outLen = rows * static_cast<size_t>(rowBytes);
        return true;
    }

    return true;
}

static bool unfilterPdfPredictor(
    const uint8_t *src, size_t srcLen,
    int predictor, int columns, int colors, int bpc,
    std::vector<uint8_t> &outData)
{
    outData.assign(src, src + srcLen);
    size_t outLen = 0;
    if (!unfilterPdfPredictorInPlace(outData.data(), outData.size(), predictor, columns, colors, bpc, outLen)) {
        return false;
    }
    outData.resize(outLen);
    return true;
}

} // namespace

bool isPdfFilename(const std::string &path) {
    std::string base = baseNameOf(path);
    if (base.empty() || base[0] == '.') {
        return false;
    }
    const char *ext = strrchr(base.c_str(), '.');
    return ext && strcasecmp(ext, ".pdf") == 0;
}

struct PdfArchive::PdfImpl {
    struct XRefEntry {
        uint8_t type = 0;   // 0 = free, 1 = uncompressed at byte offset field1, 2 = compressed in ObjStm field1 at index field2
        int64_t field1 = 0;
        int field2 = 0;
    };

    int fd = -1;
    uint8_t *dmaBlockBuf = nullptr;
    int64_t cachedBlockAddr = -1;
    size_t cachedBlockLen = 0;
    int ioRetryLogCount = 0;
    std::string filePath;
    int64_t fileSize = 0;
    int64_t curPos = -1;
    std::unordered_map<int, XRefEntry> xref;
    std::unordered_map<int, std::string> objStmCache;
    std::vector<int> objStmOrder;
    std::unordered_set<int> loadedObjStms;
    std::unordered_map<int, std::string> pageInheritedRes;
    std::unordered_map<int, std::string> pageInheritedBox;
    std::unordered_map<int, int> pageInheritedRotate;

    struct FontResource {
        std::string baseFont;
        std::string subtype;
        bool isTwoByte = false;
        bool isBold = false;
        bool isItalic = false;
        bool useMacRoman = false;
        int firstChar = 0;
        int defaultWidth = 0; // in 1/1000 em
        std::vector<int> widths;
        std::unordered_map<uint16_t, int> cidWidths;
        std::unordered_map<uint16_t, std::vector<uint32_t>> toUnicode;
        std::unordered_map<uint16_t, uint32_t> encodingDiffs;
        std::unordered_map<uint16_t, std::string> encodingNames;
        std::vector<uint16_t> cidToGid;
        PdfTrueTypeFont ttf;
    };
    std::unordered_map<int, FontResource> fontObjCache;
    std::vector<std::string> *diagLog = nullptr;

    void logDiag(const std::string &msg) {
        ESP_LOGI(TAG, "%s", msg.c_str());
        if (diagLog && diagLog->size() < 48) {
            diagLog->push_back(msg);
        }
    }

    static constexpr size_t DMA_BLOCK_SIZE = 4096;

    PdfImpl() {
        ensureDmaBlockBuf();
    }
    ~PdfImpl() {
        closeFile();
        if (dmaBlockBuf) {
            free(dmaBlockBuf);
            dmaBlockBuf = nullptr;
        }
    }

    void ensureDmaBlockBuf() {
        if (dmaBlockBuf) return;
#ifdef ESP_PLATFORM
        dmaBlockBuf = static_cast<uint8_t *>(
            heap_caps_malloc(DMA_BLOCK_SIZE, MALLOC_CAP_DMA));
        if (!dmaBlockBuf) {
            dmaBlockBuf = static_cast<uint8_t *>(
                heap_caps_malloc(DMA_BLOCK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        }
#endif
        if (!dmaBlockBuf) {
            dmaBlockBuf = static_cast<uint8_t *>(malloc(DMA_BLOCK_SIZE));
        }
    }

    bool isFileOpen() const {
        return fd >= 0;
    }

    void closeFile() {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
        curPos = -1;
        cachedBlockAddr = -1;
        cachedBlockLen = 0;
    }

    bool reopenFile() {
        closeFile();
        if (filePath.empty()) return false;
#ifdef ESP_PLATFORM
        vTaskDelay(2);
#endif
#ifdef O_BINARY
        fd = ::open(filePath.c_str(), O_RDONLY | O_BINARY);
#else
        fd = ::open(filePath.c_str(), O_RDONLY);
#endif
        curPos = (fd >= 0) ? 0 : -1;
        return fd >= 0;
    }

    void clearAll() {
        closeFile();
        filePath.clear();
        fileSize = 0;
        curPos = -1;
        ioRetryLogCount = 0;
        xref.clear();
        objStmCache.clear();
        objStmOrder.clear();
        loadedObjStms.clear();
        pageInheritedRes.clear();
        pageInheritedBox.clear();
        pageInheritedRotate.clear();
        fontObjCache.clear();
    }

    // Resilient sector-aligned random-access reader using direct POSIX fd + a 4096-byte internal DMA buffer:
    // 1. Bypasses Newlib FILE* buffered I/O (eliminating Newlib's 1024-byte PSRAM _bf._base buffer).
    // 2. Aligns all lseek() calls to 4096-byte sector boundaries so FatFs f_lseek() never pre-reads
    //    unaligned sectors into fp->buf.
    // 3. Reads 4096-byte sectors into internal DMA-capable SRAM (dmaBlockBuf) and caches the active 4KB block.
    size_t readAt(int64_t offset, void *dst, size_t len) {
        if (!dst || len == 0 || offset < 0 || offset >= fileSize) return 0;
        ensureDmaBlockBuf();
        if (!dmaBlockBuf) return 0;
        if (fd < 0 && !reopenFile()) return 0;

        size_t maxAvail = static_cast<size_t>(fileSize - offset);
        size_t toRead = std::min(len, maxAvail);
        uint8_t *outPtr = static_cast<uint8_t *>(dst);
        size_t totalRead = 0;

        while (totalRead < toRead) {
            int64_t curOffset = offset + static_cast<int64_t>(totalRead);
            int64_t blockAddr = curOffset & ~static_cast<int64_t>(DMA_BLOCK_SIZE - 1);
            size_t inBlockOff = static_cast<size_t>(curOffset - blockAddr);

            if (cachedBlockAddr != blockAddr || cachedBlockLen <= inBlockOff) {
                size_t expectedBlockBytes = static_cast<size_t>(
                    std::min<int64_t>( static_cast<int64_t>(DMA_BLOCK_SIZE), fileSize - blockAddr ));
                bool blockOk = false;

                for (int attempt = 0; attempt < 3; ++attempt) {
                    if (fd < 0 && !reopenFile()) break;
                    if (curPos != blockAddr) {
                        off_t seekRes = ::lseek(fd, static_cast<off_t>(blockAddr), SEEK_SET);
                        if (seekRes != static_cast<off_t>(blockAddr)) {
                            if (ioRetryLogCount < 4) {
                                ioRetryLogCount++;
                                logDiag("lseek_retry off=" + std::to_string(blockAddr) + " err=" + std::to_string(errno));
                            }
                            reopenFile();
                            continue;
                        }
                        curPos = blockAddr;
                    }

                    size_t got = 0;
                    bool readErr = false;
                    while (got < expectedBlockBytes) {
                        ssize_t r = ::read(fd, dmaBlockBuf + got, expectedBlockBytes - got);
                        if (r <= 0) {
                            readErr = true;
                            break;
                        }
                        got += static_cast<size_t>(r);
                        curPos += static_cast<int64_t>(r);
                    }

                    if (got > inBlockOff && (!readErr || got == expectedBlockBytes)) {
                        cachedBlockAddr = blockAddr;
                        cachedBlockLen = got;
                        blockOk = true;
                        break;
                    }

                    if (ioRetryLogCount < 4) {
                        ioRetryLogCount++;
                        logDiag("read_retry off=" + std::to_string(blockAddr) +
                                " got=" + std::to_string(got) + " err=" + std::to_string(errno));
                    }
                    reopenFile();
                }

                if (!blockOk) break;
            }

            size_t availInBlock = cachedBlockLen - inBlockOff;
            size_t copyLen = std::min(toRead - totalRead, availInBlock);
            memcpy(outPtr + totalRead, dmaBlockBuf + inBlockOff, copyLen);
            totalRead += copyLen;
        }
        return totalRead;
    }

    std::string readStringAt(int64_t offset, size_t maxLen) {
        if (offset < 0 || offset >= fileSize || maxLen == 0) return "";
        size_t toRead = std::min(maxLen, static_cast<size_t>(fileSize - offset));
        std::string buf(toRead, '\0');
        size_t n = readAt(offset, &buf[0], toRead);
        buf.resize(n);
        return buf;
    }

    bool readBytesAt(int64_t offset, uint8_t *dst, size_t len) {
        if (!dst || offset < 0 || offset + static_cast<int64_t>(len) > fileSize) return false;
        return readAt(offset, dst, len) == len;
    }

    // Reads the dictionary "<< ... >>" and optional "stream" offset for a top-level indirect object at objOffset
    bool readDictAndStreamAt(int64_t objOffset, std::string &outDict, int64_t &outStreamOffset) {
        outDict.clear();
        outStreamOffset = -1;
        if (objOffset < 0 || objOffset >= fileSize) return false;

        // Fast 1KB read first (covers >99% of PDF object dictionaries); expand up to 64KB if needed
        std::string chunk = readStringAt(objOffset, 1024);
        if (chunk.empty()) return false;

        // Advance past "<id> <gen> obj" header if present so an earlier "endobj" cannot interfere
        size_t searchFrom = 0;
        size_t objHdr = chunk.find("obj");
        if (objHdr != std::string::npos && objHdr < 64) {
            searchFrom = objHdr + 3;
        }

        size_t dictStart = chunk.find("<<", searchFrom);
        if (dictStart == std::string::npos) return false;

        // Ensure "endobj" or "stream" did not appear between "obj" and "<<"
        size_t preEndObj = chunk.find("endobj", searchFrom);
        if (preEndObj != std::string::npos && preEndObj < dictStart) return false;
        size_t preStream = chunk.find("stream", searchFrom);
        if (preStream != std::string::npos && preStream < dictStart) return false;

        size_t dictEnd = findMatchingDictEnd(chunk.c_str(), chunk.size(), dictStart);
        if (dictEnd == std::string::npos && chunk.size() == 1024) {
            chunk = readStringAt(objOffset, 16384);
            dictEnd = findMatchingDictEnd(chunk.c_str(), chunk.size(), dictStart);
            if (dictEnd == std::string::npos && chunk.size() == 16384) {
                chunk = readStringAt(objOffset, 65536);
                dictEnd = findMatchingDictEnd(chunk.c_str(), chunk.size(), dictStart);
            }
        }
        if (dictEnd == std::string::npos) return false;

        outDict = chunk.substr(dictStart, dictEnd - dictStart);

        // Strictly check if "stream" immediately follows ">>" after PDF whitespace
        if (dictEnd + 16 > chunk.size() && objOffset + static_cast<int64_t>(dictEnd) < fileSize) {
            std::string tail = readStringAt(objOffset + static_cast<int64_t>(dictEnd), 32);
            const char *p = tail.c_str();
            const char *end = p + tail.size();
            skipPdfWhitespace(p, end);
            if (end - p >= 6 && memcmp(p, "stream", 6) == 0) {
                p += 6;
                if (p < end && *p == '\r') p++;
                if (p < end && *p == '\n') p++;
                outStreamOffset = objOffset + static_cast<int64_t>(dictEnd) + static_cast<int64_t>(p - tail.c_str());
            }
        } else {
            const char *p = chunk.c_str() + dictEnd;
            const char *end = chunk.c_str() + chunk.size();
            skipPdfWhitespace(p, end);
            if (end - p >= 6 && memcmp(p, "stream", 6) == 0) {
                p += 6;
                if (p < end && *p == '\r') p++;
                if (p < end && *p == '\n') p++;
                outStreamOffset = objOffset + static_cast<int64_t>(p - chunk.c_str());
            }
        }
        return true;
    }

    int64_t resolveStreamLength(int64_t streamOffset, const std::string &dict) {
        std::string lenVal = extractDictValue(dict, "Length");
        if (!lenVal.empty()) {
            int refId = parseRefObjId(lenVal);
            if (refId > 0) {
                std::string lenContent = getObjectContent(refId);
                const char *p = lenContent.c_str();
                while (*p && !std::isdigit(static_cast<unsigned char>(*p))) p++;
                if (*p) {
                    long long l = strtoll(p, nullptr, 10);
                    if (l > 0) return static_cast<int64_t>(l);
                }
            } else {
                const char *p = lenVal.c_str();
                while (*p && !std::isdigit(static_cast<unsigned char>(*p))) p++;
                if (*p) {
                    long long l = strtoll(p, nullptr, 10);
                    if (l > 0) return static_cast<int64_t>(l);
                }
            }
        }

        // Fallback: scan for "endstream"
        if (streamOffset > 0 && streamOffset < fileSize) {
            char scanBuf[4096];
            int64_t curr = streamOffset;
            while (curr < fileSize) {
                size_t n = readAt(curr, scanBuf, sizeof(scanBuf));
                if (n < 9) break;
                for (size_t i = 0; i + 9 <= n; ++i) {
                    if (memcmp(&scanBuf[i], "endstream", 9) == 0) {
                        int64_t endPos = curr + static_cast<int64_t>(i);
                        while (endPos > streamOffset && i > 0 &&
                               (scanBuf[i - 1] == '\n' || scanBuf[i - 1] == '\r')) {
                            endPos--;
                            i--;
                        }
                        return endPos - streamOffset;
                    }
                }
                curr += static_cast<int64_t>(n) - 8;
            }
        }
        return 0;
    }

    void extractPredictorParms(const std::string &dict, int &outPredictor, int &outColumns, int &outColors, int &outBpc) {
        outPredictor = 1;
        outColumns = 0;
        std::string dp = extractDictValue(dict, "DecodeParms");
        if (dp.empty()) dp = extractDictValue(dict, "DP");
        if (!dp.empty()) {
            int dpRef = parseRefObjId(dp);
            if (dpRef > 0) {
                std::string resolvedDp;
                if (getObjectDict(dpRef, resolvedDp)) dp = resolvedDp;
            } else if (dp.front() == '[') {
                size_t subStart = dp.find("<<");
                if (subStart != std::string::npos) {
                    size_t subEnd = findMatchingDictEnd(dp.c_str(), dp.size(), subStart);
                    if (subEnd != std::string::npos) {
                        dp = dp.substr(subStart, subEnd - subStart);
                    }
                }
            }
            std::string predVal = extractDictValue(dp, "Predictor");
            std::string colVal = extractDictValue(dp, "Columns");
            std::string clrVal = extractDictValue(dp, "Colors");
            std::string bpcVal = extractDictValue(dp, "BitsPerComponent");
            if (!predVal.empty()) outPredictor = atoi(predVal.c_str());
            if (!colVal.empty()) outColumns = atoi(colVal.c_str());
            if (!clrVal.empty()) outColors = atoi(clrVal.c_str());
            if (!bpcVal.empty()) outBpc = atoi(bpcVal.c_str());
        }
    }

    bool applyFilterChain(const std::string &dict, const uint8_t *raw, size_t rawLen, std::vector<uint8_t> &outData) {
        outData.clear();
        if (!raw || rawLen == 0) return false;

        std::string filter = extractDictValue(dict, "Filter");
        if (filter.empty()) filter = extractDictValue(dict, "F");
        int filterRef = parseRefObjId(filter);
        if (filterRef > 0) filter = getObjectContent(filterRef);

        if (filter.empty()) {
            outData.assign(raw, raw + rawLen);
            return true;
        }

        // Parse filter names in order (single /Name or [/Name1 /Name2 ...])
        std::vector<std::string> filters;
        const char *fp = filter.c_str();
        const char *fend = fp + filter.size();
        while (fp < fend) {
            if (*fp == '/') {
                fp++;
                const char *ns = fp;
                while (fp < fend && !isPdfWhitespace(*fp) && !isPdfDelimiter(*fp)) fp++;
                filters.emplace_back(ns, fp);
            } else {
                fp++;
            }
        }
        if (filters.empty()) {
            filters.push_back(filter);
        }

        std::vector<uint8_t> curBuf;
        std::vector<uint8_t> nextBuf;

        for (size_t fi = 0; fi < filters.size(); ++fi) {
            const std::string &f = filters[fi];
            const uint8_t *inPtr = (fi == 0) ? raw : curBuf.data();
            size_t inLen = (fi == 0) ? rawLen : curBuf.size();

            // Stop before DCTDecode if this is a Flate+DCT chain (handled by JPEG decoder)
            if (f == "DCTDecode" || f == "DCT") {
                if (fi == 0) curBuf.assign(raw, raw + rawLen);
                break;
            }
            nextBuf.clear();
            if (f == "ASCIIHexDecode" || f == "AHx") {
                if (!decodePdfAsciiHex(inPtr, inLen, nextBuf)) return false;
                curBuf.swap(nextBuf);
            } else if (f == "ASCII85Decode" || f == "A85") {
                if (!decodePdfAscii85(inPtr, inLen, nextBuf)) return false;
                curBuf.swap(nextBuf);
            } else if (f == "RunLengthDecode" || f == "RL") {
                if (!decodePdfRunLength(inPtr, inLen, nextBuf)) return false;
                curBuf.swap(nextBuf);
            } else if (f == "LZWDecode" || f == "LZW") {
                int earlyChange = 1;
                std::string dp = extractDictValue(dict, "DecodeParms");
                if (dp.empty()) dp = extractDictValue(dict, "DP");
                if (!dp.empty()) {
                    std::string ecStr = extractDictValue(dp, "EarlyChange");
                    if (!ecStr.empty()) earlyChange = atoi(ecStr.c_str());
                }
                if (!decodePdfLzw(inPtr, inLen, earlyChange, nextBuf)) return false;
                int predictor = 1, columns = 0, colors = 1, bpc = 8;
                extractPredictorParms(dict, predictor, columns, colors, bpc);
                if (predictor > 1 && columns > 0) {
                    size_t unfiltLen = 0;
                    if (unfilterPdfPredictorInPlace(nextBuf.data(), nextBuf.size(), predictor, columns, colors, bpc, unfiltLen)) {
                        nextBuf.resize(unfiltLen);
                    }
                }
                curBuf.swap(nextBuf);
            } else if (f == "FlateDecode" || f == "Fl") {
                size_t uncompLen = 0;
                uint8_t *uncomp = inflatePdfStream(inPtr, inLen, uncompLen);
                if (!uncomp || uncompLen == 0) {
                    if (uncomp) free(uncomp);
                    return false;
                }
                curBuf.clear();
                curBuf.shrink_to_fit();
                int predictor = 1, columns = 0, colors = 1, bpc = 8;
                extractPredictorParms(dict, predictor, columns, colors, bpc);
                if (predictor > 1 && columns > 0) {
                    size_t unfiltLen = 0;
                    if (unfilterPdfPredictorInPlace(uncomp, uncompLen, predictor, columns, colors, bpc, unfiltLen)) {
                        uncompLen = unfiltLen;
                    }
                }
                nextBuf.assign(uncomp, uncomp + uncompLen);
                free(uncomp);
                curBuf.swap(nextBuf);
            } else if (fi == 0) {
                curBuf.assign(raw, raw + rawLen);
            }
        }

        outData.swap(curBuf);
        return !outData.empty();
    }

    bool readDecompressedStream(const std::string &dict, int64_t streamOffset, std::vector<uint8_t> &outData) {
        outData.clear();
        if (streamOffset <= 0 || streamOffset >= fileSize) return false;
        int64_t streamLen = resolveStreamLength(streamOffset, dict);
        if (streamLen <= 0 || streamLen > 16 * 1024 * 1024) return false;

        uint8_t *raw = allocComicPageBuffer(static_cast<size_t>(streamLen));
        if (!raw) return false;
        if (!readBytesAt(streamOffset, raw, static_cast<size_t>(streamLen))) {
            free(raw);
            return false;
        }

        bool ok = applyFilterChain(dict, raw, static_cast<size_t>(streamLen), outData);
        free(raw);
        return ok;
    }

    bool unpackObjStmData(int objStmId, int nObjs, int firstOff, const std::vector<uint8_t> &dec) {
        if (nObjs <= 0 || firstOff <= 0 || dec.size() <= static_cast<size_t>(firstOff)) {
            return false;
        }
        loadedObjStms.insert(objStmId);

        // Parse N pairs of (objNum, byteOffsetRelativeToFirst)
        const char *p = reinterpret_cast<const char *>(dec.data());
        const char *hdrEnd = p + firstOff;
        std::vector<std::pair<int, int>> pairs;
        pairs.reserve(nObjs);

        for (int i = 0; i < nObjs; ++i) {
            skipPdfWhitespace(p, hdrEnd);
            if (p >= hdrEnd) break;
            char *endp = nullptr;
            long oid = strtol(p, &endp, 10);
            if (endp == p) break;
            p = endp;
            skipPdfWhitespace(p, hdrEnd);
            if (p >= hdrEnd) break;
            long relOff = strtol(p, &endp, 10);
            if (endp == p) break;
            p = endp;
            pairs.push_back({ static_cast<int>(oid), static_cast<int>(relOff) });
        }

        // Sort pairs by relative offset so out-of-order ObjStm entries are sliced accurately
        std::sort(pairs.begin(), pairs.end(), [](const std::pair<int, int> &a, const std::pair<int, int> &b) {
            return a.second < b.second;
        });

        // Bound objStmCache size to protect heap on huge PDF 1.5+ books while keeping typical 1500-object magazines intact
        if (objStmCache.size() > 4096 && objStmCache.size() + pairs.size() > 4608) {
            objStmCache.clear();
            objStmOrder.clear();
            loadedObjStms.clear();
            loadedObjStms.insert(objStmId);
        }

        for (size_t i = 0; i < pairs.size(); ++i) {
            int oid = pairs[i].first;
            size_t startPos = static_cast<size_t>(firstOff) + static_cast<size_t>(std::max(0, pairs[i].second));
            size_t endPos = (i + 1 < pairs.size())
                ? (static_cast<size_t>(firstOff) + static_cast<size_t>(std::max(0, pairs[i + 1].second)))
                : dec.size();
            if (startPos < dec.size() && endPos > startPos && endPos <= dec.size()) {
                if (xref.find(oid) == xref.end()) {
                    xref[oid] = { 2, objStmId, static_cast<int>(i) };
                }
                if (objStmCache.find(oid) == objStmCache.end()) {
                    std::string objStr(reinterpret_cast<const char *>(dec.data() + startPos), endPos - startPos);
                    std::string tVal = extractDictValue(objStr, "Type");
                    if (tVal != "/Annot" && tVal != "Annot" && tVal != "/StructElem" && tVal != "StructElem") {
                        objStmCache[oid] = std::move(objStr);
                        objStmOrder.push_back(oid);
                    }
                }
            }
        }
        return true;
    }

    // Fast in-RAM ObjStm unpacker used during sequential chunk scanning when the entire ObjStm fits in the active buffer
    bool tryLoadObjStmFromBuffer(int objStmId, const char *objStart, const char *bufEnd) {
        if (loadedObjStms.count(objStmId)) return true;
        if (!objStart || objStart >= bufEnd) return false;

        size_t avail = static_cast<size_t>(bufEnd - objStart);
        std::string hdrView(objStart, std::min<size_t>(avail, 2048));
        size_t searchFrom = 0;
        size_t objHdr = hdrView.find("obj");
        if (objHdr != std::string::npos && objHdr < 64) {
            searchFrom = objHdr + 3;
        }
        size_t dictStart = hdrView.find("<<", searchFrom);
        if (dictStart == std::string::npos) return false;
        size_t dictEnd = findMatchingDictEnd(hdrView.c_str(), hdrView.size(), dictStart);
        if (dictEnd == std::string::npos) return false;

        std::string dict = hdrView.substr(dictStart, dictEnd - dictStart);
        int nObjs = atoi(extractDictValue(dict, "N").c_str());
        int firstOff = atoi(extractDictValue(dict, "First").c_str());
        if (nObjs <= 0 || firstOff <= 0) return false;

        const char *p = objStart + dictEnd;
        skipPdfWhitespace(p, bufEnd);
        if (bufEnd - p < 6 || memcmp(p, "stream", 6) != 0) return false;
        p += 6;
        if (p < bufEnd && *p == '\r') p++;
        if (p < bufEnd && *p == '\n') p++;

        int64_t streamLen = 0;
        std::string lenVal = extractDictValue(dict, "Length");
        if (!lenVal.empty() && parseRefObjId(lenVal) <= 0) {
            const char *lp = lenVal.c_str();
            while (*lp && !std::isdigit(static_cast<unsigned char>(*lp))) lp++;
            if (*lp) streamLen = strtoll(lp, nullptr, 10);
        }
        if (streamLen <= 0) {
            const char *es = p;
            while (es + 9 <= bufEnd) {
                const char *ePos = static_cast<const char *>(memchr(es, 'e', static_cast<size_t>((bufEnd - 9) - es + 1)));
                if (!ePos) break;
                if (memcmp(ePos, "endstream", 9) == 0) {
                    const char *endPtr = ePos;
                    while (endPtr > p && (endPtr[-1] == '\n' || endPtr[-1] == '\r')) endPtr--;
                    streamLen = static_cast<int64_t>(endPtr - p);
                    break;
                }
                es = ePos + 1;
            }
        }
        if (streamLen <= 0 || p + streamLen > bufEnd) return false;

        std::vector<uint8_t> dec;
        if (!applyFilterChain(dict, reinterpret_cast<const uint8_t *>(p), static_cast<size_t>(streamLen), dec)) {
            return false;
        }
        return unpackObjStmData(objStmId, nObjs, firstOff, dec);
    }

    bool loadObjStm(int objStmId) {
        if (loadedObjStms.count(objStmId)) return true;

        auto it = xref.find(objStmId);
        if (it == xref.end() || it->second.type != 1 || it->second.field1 <= 0) {
            return false;
        }

        std::string dict;
        int64_t streamOff = -1;
        if (!readDictAndStreamAt(it->second.field1, dict, streamOff) || streamOff <= 0) {
            return false;
        }

        int nObjs = atoi(extractDictValue(dict, "N").c_str());
        int firstOff = atoi(extractDictValue(dict, "First").c_str());
        if (nObjs <= 0 || firstOff <= 0) return false;

        std::vector<uint8_t> dec;
        if (!readDecompressedStream(dict, streamOff, dec) || dec.size() <= static_cast<size_t>(firstOff)) {
            return false;
        }
        return unpackObjStmData(objStmId, nObjs, firstOff, dec);
    }

    // Resolves any object's dictionary (whether uncompressed on disk or compressed inside an ObjStm)
    bool getObjectDict(int objId, std::string &outDict, int64_t *outStreamOffset = nullptr) {
        outDict.clear();
        if (outStreamOffset) *outStreamOffset = -1;
        if (objId <= 0) return false;

        auto cacheIt = objStmCache.find(objId);
        if (cacheIt != objStmCache.end()) {
            const std::string &s = cacheIt->second;
            size_t dStart = s.find("<<");
            if (dStart == std::string::npos) return false;
            size_t dEnd = findMatchingDictEnd(s.c_str(), s.size(), dStart);
            if (dEnd == std::string::npos) return false;
            outDict = s.substr(dStart, dEnd - dStart);
            return true;
        }

        auto it = xref.find(objId);
        if (it == xref.end()) return false;

        if (it->second.type == 2) {
            int stmId = static_cast<int>(it->second.field1);
            if (!loadObjStm(stmId)) return false;
            auto cacheIt2 = objStmCache.find(objId);
            if (cacheIt2 == objStmCache.end()) return false;
            const std::string &s = cacheIt2->second;
            size_t dStart = s.find("<<");
            if (dStart == std::string::npos) return false;
            size_t dEnd = findMatchingDictEnd(s.c_str(), s.size(), dStart);
            if (dEnd == std::string::npos) return false;
            outDict = s.substr(dStart, dEnd - dStart);
            return true;
        } else if (it->second.type == 1) {
            int64_t streamOff = -1;
            if (!readDictAndStreamAt(it->second.field1, outDict, streamOff)) return false;
            if (outStreamOffset) *outStreamOffset = streamOff;
            return true;
        }
        return false;
    }

    // Resolves non-dictionary object content (e.g. indirect integer /Length, indirect array /Kids or /ColorSpace)
    std::string getObjectContent(int objId) {
        if (objId <= 0) return "";
        auto cacheIt = objStmCache.find(objId);
        if (cacheIt != objStmCache.end()) {
            return cacheIt->second;
        }

        auto it = xref.find(objId);
        if (it == xref.end()) return "";

        if (it->second.type == 2) {
            if (loadObjStm(static_cast<int>(it->second.field1))) {
                auto cacheIt2 = objStmCache.find(objId);
                if (cacheIt2 != objStmCache.end()) return cacheIt2->second;
            }
            return "";
        } else if (it->second.type == 1 && it->second.field1 >= 0) {
            std::string raw = readStringAt(it->second.field1, 2048);
            size_t objPos = raw.find("obj");
            size_t bodyStart = (objPos != std::string::npos && objPos < 64) ? (objPos + 3) : 0;
            size_t endObjPos = raw.find("endobj", bodyStart);
            if (endObjPos == std::string::npos && raw.size() == 2048 && raw.find("stream", bodyStart) == std::string::npos) {
                raw = readStringAt(it->second.field1, 65536);
                endObjPos = raw.find("endobj", bodyStart);
            }
            if (endObjPos != std::string::npos) {
                return raw.substr(bodyStart, endObjPos - bodyStart);
            }
            return raw.substr(bodyStart);
        }
        return "";
    }

    // Parses an xref table or PDF 1.5+ /Type /XRef stream at xrefPos, following /XRefStm and /Prev
    bool parseXRefChain(int64_t startPos, int &outRootId, int &outInfoId, std::vector<std::string> &parseLog) {
        std::unordered_set<int64_t> visited;
        std::vector<int64_t> queue = { startPos };

        while (!queue.empty() && visited.size() < 32) {
            int64_t pos = queue.back();
            queue.pop_back();
            if (pos <= 0 || pos >= fileSize || visited.count(pos)) continue;
            visited.insert(pos);

            std::string head = readStringAt(pos, 128);
            const char *p = head.c_str();
            const char *end = p + head.size();
            skipPdfWhitespace(p, end);

            if (!(end - p >= 4 && memcmp(p, "xref", 4) == 0 && (end - p == 4 || isPdfWhitespace(p[4]))) &&
                head.find("obj") == std::string::npos)
            {
                // Linearized PDFs set /T to the first xref entry after "xref\r\n0 N\r\n"; check 32 bytes earlier
                int64_t backOff = std::max<int64_t>(0, pos - 32);
                std::string pre = readStringAt(backOff, 64);
                size_t xPos = pre.find("xref");
                if (xPos != std::string::npos) {
                    pos = backOff + static_cast<int64_t>(xPos);
                    head = readStringAt(pos, 128);
                    p = head.c_str();
                    end = p + head.size();
                    skipPdfWhitespace(p, end);
                }
            }

            if (end - p >= 4 && memcmp(p, "xref", 4) == 0 && (end - p == 4 || isPdfWhitespace(p[4]))) {
                // Traditional ASCII cross-reference table
                int64_t scanStart = pos + static_cast<int64_t>((p - head.c_str()) + 4);
                char lineBuf[256];
                int64_t trailerPos = -1;
                int64_t cursor = scanStart;
                std::string xChunk;
                int64_t xChunkStart = scanStart;

                // Read lines supporting LF (\n), CRLF (\r\n), and classic Mac CR (\r) line endings
                // using 8KB buffered reads in RAM (avoiding per-line fseek/ftell on FatFs),
                // while preserving the exact file offset where "trailer" starts (even when "trailer <<" is on one line).
                auto readXrefLine = [&](char *buf, size_t cap, int64_t &outLinePos) -> bool {
                    if (cursor < 0 || cursor >= fileSize || cap < 2) return false;
                    outLinePos = cursor;
                    if (cursor < xChunkStart ||
                        cursor + static_cast<int64_t>(cap) > xChunkStart + static_cast<int64_t>(xChunk.size()))
                    {
                        if (xChunkStart + static_cast<int64_t>(xChunk.size()) < fileSize ||
                            cursor < xChunkStart ||
                            cursor >= xChunkStart + static_cast<int64_t>(xChunk.size()))
                        {
                            xChunkStart = cursor;
                            xChunk = readStringAt(xChunkStart, 8192);
                        }
                    }
                    if (cursor < xChunkStart || cursor >= xChunkStart + static_cast<int64_t>(xChunk.size())) {
                        return false;
                    }
                    size_t rel = static_cast<size_t>(cursor - xChunkStart);
                    size_t n = std::min(cap - 1, xChunk.size() - rel);
                    if (n == 0) return false;
                    const char *src = xChunk.data() + rel;
                    size_t len = 0;
                    while (len < n && src[len] != '\n' && src[len] != '\r') {
                        len++;
                    }
                    size_t step = len;
                    if (step < n) {
                        if (src[step] == '\r' && step + 1 < n && src[step + 1] == '\n') {
                            step += 2;
                        } else {
                            step += 1;
                        }
                    }
                    memcpy(buf, src, len);
                    buf[len] = '\0';
                    cursor += static_cast<int64_t>(step);
                    return true;
                };

                int64_t lineStartPos = scanStart;
                while (readXrefLine(lineBuf, sizeof(lineBuf), lineStartPos)) {
                    const char *lp = lineBuf;
                    while (*lp && isPdfWhitespace(*lp)) lp++;
                    if (strncmp(lp, "trailer", 7) == 0 &&
                        (lp[7] == '\0' || isPdfWhitespace(lp[7]) || isPdfDelimiter(lp[7])))
                    {
                        trailerPos = lineStartPos + static_cast<int64_t>(lp - lineBuf);
                        break;
                    }

                    int startId = 0, count = 0;
                    if (sscanf(lp, "%d %d", &startId, &count) == 2 && count > 0 && count < 100000) {
                        for (int i = 0; i < count; ++i) {
                            int64_t entryLinePos = 0;
                            if (!readXrefLine(lineBuf, sizeof(lineBuf), entryLinePos)) break;
                            const char *ep = lineBuf;
                            while (*ep && isPdfWhitespace(*ep)) ep++;
                            if (strncmp(ep, "trailer", 7) == 0 &&
                                (ep[7] == '\0' || isPdfWhitespace(ep[7]) || isPdfDelimiter(ep[7])))
                            {
                                trailerPos = entryLinePos + static_cast<int64_t>(ep - lineBuf);
                                break;
                            }
                            long long offset = 0;
                            int gen = 0;
                            char type = 'f';
                            if (sscanf(lineBuf, "%lld %d %c", &offset, &gen, &type) == 3) {
                                int oid = startId + i;
                                if (xref.find(oid) == xref.end()) {
                                    if (type == 'n' && offset > 0) {
                                        xref[oid] = { 1, static_cast<int64_t>(offset), gen };
                                    } else if (type == 'f') {
                                        xref[oid] = { 0, 0, gen };
                                    }
                                }
                            }
                        }
                        if (trailerPos > 0) break;
                    }
                }

                if (trailerPos < 0) {
                    trailerPos = cursor;
                }
                std::string trailerChunk = readStringAt(trailerPos, 4096);
                size_t dStart = trailerChunk.find("<<");
                if (dStart != std::string::npos) {
                    size_t dEnd = findMatchingDictEnd(trailerChunk.c_str(), trailerChunk.size(), dStart);
                    std::string trailerDict = (dEnd != std::string::npos)
                        ? trailerChunk.substr(dStart, dEnd - dStart)
                        : trailerChunk.substr(dStart);

                    if (outRootId <= 0) outRootId = parseRefObjId(extractDictValue(trailerDict, "Root"));
                    if (outInfoId <= 0) outInfoId = parseRefObjId(extractDictValue(trailerDict, "Info"));

                    std::string prevVal = extractDictValue(trailerDict, "Prev");
                    if (!prevVal.empty()) {
                        int64_t prevOff = strtoll(prevVal.c_str(), nullptr, 10);
                        if (prevOff > 0 && prevOff < fileSize) queue.push_back(prevOff);
                    }
                    std::string xrefStmVal = extractDictValue(trailerDict, "XRefStm");
                    if (!xrefStmVal.empty()) {
                        int64_t stmOff = strtoll(xrefStmVal.c_str(), nullptr, 10);
                        if (stmOff > 0 && stmOff < fileSize) queue.push_back(stmOff);
                    }
                }
            } else {
                // PDF 1.5+ Cross-Reference Stream (/Type /XRef)
                std::string xrefDict;
                int64_t streamOff = -1;
                if (!readDictAndStreamAt(pos, xrefDict, streamOff) || streamOff <= 0) {
                    continue;
                }

                std::string wStr = extractDictValue(xrefDict, "W");
                if (wStr.empty()) continue;
                std::vector<int64_t> w = parseIntArray(wStr);
                if (w.size() < 3) continue;
                int w0 = static_cast<int>(w[0]);
                int w1 = static_cast<int>(w[1]);
                int w2 = static_cast<int>(w[2]);
                int entryWidth = w0 + w1 + w2;
                if (entryWidth <= 0 || entryWidth > 32) continue;

                if (outRootId <= 0) outRootId = parseRefObjId(extractDictValue(xrefDict, "Root"));
                if (outInfoId <= 0) outInfoId = parseRefObjId(extractDictValue(xrefDict, "Info"));

                std::string prevVal = extractDictValue(xrefDict, "Prev");
                if (!prevVal.empty()) {
                    int64_t prevOff = strtoll(prevVal.c_str(), nullptr, 10);
                    if (prevOff > 0 && prevOff < fileSize) queue.push_back(prevOff);
                }

                int totalSize = atoi(extractDictValue(xrefDict, "Size").c_str());
                std::string indexStr = extractDictValue(xrefDict, "Index");
                std::vector<int64_t> indexArr;
                if (!indexStr.empty()) {
                    indexArr = parseIntArray(indexStr);
                }
                if (indexArr.size() < 2) {
                    indexArr = { 0, std::max(0, totalSize) };
                }

                std::vector<uint8_t> dec;
                if (!readDecompressedStream(xrefDict, streamOff, dec) || dec.empty()) {
                    parseLog.push_back("xref_stream_decompress_failed_at=" + std::to_string(pos));
                    continue;
                }

                size_t bytePos = 0;
                for (size_t s = 0; s + 1 < indexArr.size(); s += 2) {
                    int firstObjId = static_cast<int>(indexArr[s]);
                    int count = static_cast<int>(indexArr[s + 1]);
                    for (int i = 0; i < count; ++i) {
                        if (bytePos + static_cast<size_t>(entryWidth) > dec.size()) break;
                        const uint8_t *eb = dec.data() + bytePos;
                        bytePos += static_cast<size_t>(entryWidth);

                        int type = (w0 == 0) ? 1 : 0;
                        for (int b = 0; b < w0; ++b) type = (type << 8) | eb[b];

                        int64_t f1 = 0;
                        for (int b = 0; b < w1; ++b) f1 = (f1 << 8) | eb[w0 + b];

                        int f2 = 0;
                        for (int b = 0; b < w2; ++b) f2 = (f2 << 8) | eb[w0 + w1 + b];

                        int oid = firstObjId + i;
                        if (xref.find(oid) == xref.end()) {
                            xref[oid] = { static_cast<uint8_t>(type), f1, f2 };
                        }
                    }
                }
            }
        }

        parseLog.push_back("xref_objects=" + std::to_string(xref.size()) + " root=" + std::to_string(outRootId));
        return !xref.empty();
    }

    int resolveColorComponents(const std::string &dict) {
        std::string cs = extractDictValue(dict, "ColorSpace");
        if (cs.empty()) cs = extractDictValue(dict, "CS");
        if (cs.empty()) return 3;

        int csRef = parseRefObjId(cs);
        if (csRef > 0) {
            std::string csObj = getObjectContent(csRef);
            if (!csObj.empty()) cs = csObj;
        }

        if (cs.find("DeviceGray") != std::string::npos || cs.find("/G") != std::string::npos ||
            cs.find("CalGray") != std::string::npos) {
            return 1;
        }
        if (cs.find("DeviceCMYK") != std::string::npos || cs.find("/CMYK") != std::string::npos) {
            return 4;
        }
        if (cs.find("ICCBased") != std::string::npos) {
            std::vector<int> refs = parseRefArray(cs);
            if (!refs.empty()) {
                std::string iccDict;
                if (getObjectDict(refs[0], iccDict)) {
                    int n = atoi(extractDictValue(iccDict, "N").c_str());
                    if (n == 1 || n == 3 || n == 4) return n;
                }
            }
        }
        return 3;
    }

    // Parses an Image XObject (or a Form XObject wrapping an Image XObject) into PageEntry
    bool parseImageObj(int objId, int pageObjId, PageEntry &outEntry, int depth = 0) {
        if (objId <= 0 || depth > 3) return false;
        auto it = xref.find(objId);
        if (it == xref.end() || it->second.type != 1 || it->second.field1 <= 0) {
            return false;
        }

        return parseImageObjAtOffset(objId, pageObjId, it->second.field1, outEntry, depth);
    }

    bool parseImageObjAtOffset(int objId, int pageObjId, int64_t offset, PageEntry &outEntry, int depth = 0) {
        if (depth > 3 || offset <= 0 || offset >= fileSize) return false;

        std::string dict;
        int64_t streamOff = -1;
        if (!readDictAndStreamAt(offset, dict, streamOff) || streamOff <= 0) {
            return false;
        }

        std::string subtype = extractDictValue(dict, "Subtype");
        if (subtype == "/Form" || subtype == "Form") {
            // Form XObject: check if it wraps an Image XObject in its /Resources
            std::string resStr = extractDictValue(dict, "Resources");
            int resRef = parseRefObjId(resStr);
            if (resRef > 0) getObjectDict(resRef, resStr);
            if (!resStr.empty()) {
                std::string xobjStr = extractDictValue(resStr, "XObject");
                int xobjRef = parseRefObjId(xobjStr);
                if (xobjRef > 0) getObjectDict(xobjRef, xobjStr);
                std::vector<int> subRefs = parseRefArray(xobjStr);
                PageEntry bestSub;
                bool foundSub = false;
                for (int subId : subRefs) {
                    PageEntry cand;
                    if (parseImageObj(subId, pageObjId, cand, depth + 1)) {
                        if (!foundSub || cand.streamLen > bestSub.streamLen) {
                            bestSub = cand;
                            foundSub = true;
                        }
                    }
                }
                if (foundSub) {
                    outEntry = bestSub;
                    return true;
                }
            }
            return false;
        }

        if (subtype != "/Image" && subtype != "Image") {
            return false;
        }

        int w = 0, h = 0;
        std::string wVal = extractDictValue(dict, "Width");
        if (wVal.empty()) wVal = extractDictValue(dict, "W");
        std::string hVal = extractDictValue(dict, "Height");
        if (hVal.empty()) hVal = extractDictValue(dict, "H");
        if (!wVal.empty()) {
            int wRef = parseRefObjId(wVal);
            if (wRef > 0) wVal = getObjectContent(wRef);
            w = atoi(wVal.c_str());
        }
        if (!hVal.empty()) {
            int hRef = parseRefObjId(hVal);
            if (hRef > 0) hVal = getObjectContent(hRef);
            h = atoi(hVal.c_str());
        }

        int64_t streamLen = resolveStreamLength(streamOff, dict);
        if (streamLen <= 0) {
            return false;
        }

        std::string filter = extractDictValue(dict, "Filter");
        if (filter.empty()) filter = extractDictValue(dict, "F");
        int filterRef = parseRefObjId(filter);
        if (filterRef > 0) filter = getObjectContent(filterRef);

        bool hasDct = (filter.find("DCTDecode") != std::string::npos || filter.find("/DCT") != std::string::npos);
        bool hasJbig2 = (filter.find("JBIG2Decode") != std::string::npos);
        bool hasFlate = (filter.find("FlateDecode") != std::string::npos || filter.find("/Fl") != std::string::npos ||
                         filter.find("ASCII85Decode") != std::string::npos || filter.find("/A85") != std::string::npos ||
                         filter.find("ASCIIHexDecode") != std::string::npos || filter.find("/AHx") != std::string::npos ||
                         filter.find("RunLengthDecode") != std::string::npos || filter.find("/RL") != std::string::npos ||
                         filter.find("LZWDecode") != std::string::npos || filter.find("/LZW") != std::string::npos);

        ImageFormat fmt = ImageFormat::Unknown;
        if (hasFlate && hasDct) {
            fmt = ImageFormat::FlateJpeg;
        } else if (hasDct) {
            fmt = ImageFormat::Jpeg;
        } else if (hasJbig2) {
            fmt = ImageFormat::Jbig2;
        } else if (hasFlate) {
            fmt = ImageFormat::FlateRaw;
        } else if (filter.empty()) {
            uint8_t magic[4] = {0};
            if (readBytesAt(streamOff, magic, sizeof(magic))) {
                if (magic[0] == 0xFF && magic[1] == 0xD8) {
                    fmt = ImageFormat::Jpeg;
                } else if (magic[0] == 0x89 && magic[1] == 'P' && magic[2] == 'N' && magic[3] == 'G') {
                    fmt = ImageFormat::Png;
                } else if (w > 0 && h > 0) {
                    fmt = ImageFormat::FlateRaw; // Uncompressed raw bitmap stream
                }
            }
        }

        if (fmt == ImageFormat::Unknown) {
            return false;
        }

        int colorComponents = resolveColorComponents(dict);
        int bpc = 8;
        std::string bpcVal = extractDictValue(dict, "BitsPerComponent");
        if (bpcVal.empty()) bpcVal = extractDictValue(dict, "BPC");
        if (!bpcVal.empty()) bpc = atoi(bpcVal.c_str());
        if (extractDictValue(dict, "ImageMask") == "true" || extractDictValue(dict, "IM") == "true") {
            bpc = 1;
            colorComponents = 1;
        }

        int predictor = 1, predCols = 0, predColors = colorComponents, predBpc = bpc;
        extractPredictorParms(dict, predictor, predCols, predColors, predBpc);

        outEntry.pageObjId = pageObjId;
        outEntry.imgObjId = objId;
        outEntry.streamOffset = streamOff;
        outEntry.streamLen = static_cast<size_t>(streamLen);
        outEntry.format = fmt;
        outEntry.width = w;
        outEntry.height = h;
        outEntry.bitsPerComponent = bpc;
        outEntry.colorComponents = colorComponents;
        outEntry.predictor = predictor;
        outEntry.predictorColumns = (predCols > 0) ? predCols : w;
        outEntry.resolved = true;
        return true;
    }

    // Helper: checks if a page's /Contents stream contains text ("BT") or vector path drawing.
    // Returns true unless the stream can be decompressed and verified to be a tiny (<160 byte)
    // single-image placement wrapper ("q ... cm /Im0 Do Q") with zero text or path operators.
    bool pageContentsHasTextOrVectors(const std::string &contentsVal) {
        if (contentsVal.empty()) return false;
        std::vector<int> cids;
        int singleId = parseRefObjId(contentsVal);
        if (singleId > 0) {
            std::string cObj = getObjectContent(singleId);
            if (!cObj.empty() && cObj.find('[') != std::string::npos && cObj.find("<<") == std::string::npos) {
                cids = parseRefArray(cObj);
            } else {
                cids.push_back(singleId);
            }
        } else {
            cids = parseRefArray(contentsVal);
        }
        if (cids.size() != 1) return true;

        std::string cDict;
        int64_t streamOff = -1;
        if (!getObjectDict(cids[0], cDict, &streamOff) || streamOff <= 0) return true;
        int64_t len = resolveStreamLength(streamOff, cDict);
        // Simple single-image wrapper streams ("q <w> 0 0 <h> 0 0 cm /Im0 Do Q") are < 160 bytes
        if (len <= 0 || len > 160) return true;

        std::vector<uint8_t> dec;
        if (!readDecompressedStream(cDict, streamOff, dec) || dec.empty()) return true;
        if (dec.size() > 160) return true;

        std::string s(reinterpret_cast<const char *>(dec.data()), dec.size());
        if (s.find('(') != std::string::npos || s.find('<') != std::string::npos) {
            return true;
        }
        const char *p = s.c_str();
        const char *end = p + s.size();
        while (p < end) {
            skipPdfWhitespace(p, end);
            if (p >= end) break;
            if (*p == '/') {
                p++;
                while (p < end && !isPdfWhitespace(*p) && !isPdfDelimiter(*p)) p++;
                continue;
            }
            const char *ts = p;
            while (p < end && !isPdfWhitespace(*p) && !isPdfDelimiter(*p)) p++;
            if (p == ts) { p++; continue; }
            std::string tok(ts, p);
            if (tok == "BT" || tok == "ET" || tok == "Tf" || tok == "Tj" || tok == "TJ" ||
                tok == "Td" || tok == "TD" || tok == "Tm" || tok == "T*" || tok == "'" || tok == "\"" ||
                tok == "re" || tok == "m" || tok == "l" || tok == "c" || tok == "v" || tok == "y" ||
                tok == "S" || tok == "s" || tok == "f" || tok == "F" || tok == "f*" ||
                tok == "B" || tok == "B*" || tok == "b" || tok == "b*" || tok == "BI" || tok == "sh") {
                return true;
            }
        }
        return false;
    }

    // Resolves a PageEntry from its pageObjId (or imgObjId) on demand
    bool resolvePageEntry(PageEntry &entry) {
        if (entry.resolved) return true;

        if (entry.imgObjId > 0 && entry.streamOffset == 0) {
            if (parseImageObj(entry.imgObjId, entry.pageObjId, entry)) {
                return true;
            }
        }

        if (entry.pageObjId <= 0) return false;

        // Follow down to leaf /Page if pageObjId happens to be an intermediate /Pages node
        int currId = entry.pageObjId;
        std::string nodeDict;
        for (int hop = 0; hop < 6; ++hop) {
            if (!getObjectDict(currId, nodeDict)) return false;
            std::string type = extractDictValue(nodeDict, "Type");
            if (type == "/Pages" || type == "Pages" || (!extractDictValue(nodeDict, "Kids").empty() && type != "/Page")) {
                std::string kidsStr = extractDictValue(nodeDict, "Kids");
                int kidsRef = parseRefObjId(kidsStr);
                if (kidsRef > 0) kidsStr = getObjectContent(kidsRef);
                std::vector<int> kids = parseRefArray(kidsStr);
                if (kids.empty()) return false;
                currId = kids[0];
                entry.pageObjId = currId;
            } else {
                break;
            }
        }

        std::string rotStr = extractDictValue(nodeDict, "Rotate");
        if (!rotStr.empty()) {
            entry.rotate = atoi(rotStr.c_str());
        } else {
            auto rotIt = pageInheritedRotate.find(entry.pageObjId);
            if (rotIt != pageInheritedRotate.end()) entry.rotate = rotIt->second;
        }

        // Resolve /Resources (inline, indirect, or inherited)
        std::string pageRes = extractDictValue(nodeDict, "Resources");
        if (pageRes.empty()) {
            auto inhIt = pageInheritedRes.find(entry.pageObjId);
            if (inhIt != pageInheritedRes.end()) {
                pageRes = inhIt->second;
            } else {
                int parentId = parseRefObjId(extractDictValue(nodeDict, "Parent"));
                for (int up = 0; up < 5 && parentId > 0 && pageRes.empty(); ++up) {
                    std::string parentDict;
                    if (!getObjectDict(parentId, parentDict)) break;
                    pageRes = extractDictValue(parentDict, "Resources");
                    parentId = parseRefObjId(extractDictValue(parentDict, "Parent"));
                }
            }
        }
        int resRef = parseRefObjId(pageRes);
        if (resRef > 0) getObjectDict(resRef, pageRes);

        std::string contentsVal = extractDictValue(nodeDict, "Contents");

        // Check /Font and /XObject in /Resources
        int validImageCount = 0;
        PageEntry bestPageEntry;
        bool foundImage = false;
        bool hasFormXObject = false;
        bool hasFonts = false;

        if (!pageRes.empty()) {
            std::string fontDict = extractDictValue(pageRes, "Font");
            if (!fontDict.empty() && fontDict != "<<" && fontDict != "<<>>" && fontDict != "<< >>") {
                hasFonts = true;
            }
            // Only inspect /XObject image candidates if the page does NOT already have fonts + content stream
            // (since any page with fonts + content stream uses the full ContentStream rasterizer).
            if (!hasFonts || contentsVal.empty()) {
                std::string xobjDict = extractDictValue(pageRes, "XObject");
                int xobjRef = parseRefObjId(xobjDict);
                if (xobjRef > 0) getObjectDict(xobjRef, xobjDict);

                if (!xobjDict.empty()) {
                    std::vector<int> imgRefs = parseRefArray(xobjDict);
                    for (int imgId : imgRefs) {
                        std::string subDict;
                        if (getObjectDict(imgId, subDict)) {
                            std::string st = extractDictValue(subDict, "Subtype");
                            if (st == "/Form" || st == "Form") hasFormXObject = true;
                        }
                        PageEntry cand;
                        if (parseImageObj(imgId, entry.pageObjId, cand)) {
                            validImageCount++;
                            if (!foundImage || cand.streamLen > bestPageEntry.streamLen) {
                                bestPageEntry = cand;
                                foundImage = true;
                            }
                        }
                    }
                }
            }
        }

        // Use direct single-image fast-path ONLY when the page has a single full-page image (>=400x400),
        // no /Font dictionary on the page, and no text/vector/multi-image content stream.
        if (foundImage && validImageCount == 1 && !hasFormXObject && !hasFonts &&
            bestPageEntry.width >= 400 && bestPageEntry.height >= 400 &&
            (contentsVal.empty() || !pageContentsHasTextOrVectors(contentsVal)))
        {
            bestPageEntry.rotate = entry.rotate;
            entry = bestPageEntry;
            return true;
        }

        // Otherwise, if the page has /Contents, Form XObjects, or fonts, use the full Phase 2
        // ContentStream vector + font + image rasterizer!
        if (!contentsVal.empty() || hasFormXObject || hasFonts || !foundImage) {
            entry.format = ImageFormat::ContentStream;
            entry.width = EPD_HEIGHT * 2;
            entry.height = EPD_WIDTH * 2;
            entry.resolved = true;
            return true;
        }

        if (foundImage) {
            bestPageEntry.rotate = entry.rotate;
            entry = bestPageEntry;
            return true;
        }

        return false;
    }

    // -------------------------------------------------------------------------
    // Phase 2 Full PDF Vector, TrueType Font & Composite Image Rasterizer
    // -------------------------------------------------------------------------
    void parseToUnicodeCMap(const std::string &cmapText, FontResource &outFont) {
        const char *p = cmapText.c_str();
        const char *end = p + cmapText.size();

        auto readHexBytes = [&](std::vector<uint8_t> &outBytes) -> bool {
            outBytes.clear();
            while (p < end && *p != '<') {
                if (strncmp(p, "endbf", 5) == 0 || strncmp(p, "endcodespacerange", 17) == 0) return false;
                p++;
            }
            if (p >= end || *p != '<') return false;
            p++;
            char hi = 0;
            bool haveHi = false;
            while (p < end && *p != '>') {
                char c = *p++;
                if (isPdfWhitespace(c)) continue;
                if (!haveHi) {
                    hi = c;
                    haveHi = true;
                } else {
                    char buf[3] = { hi, c, '\0' };
                    outBytes.push_back(static_cast<uint8_t>(strtoul(buf, nullptr, 16)));
                    haveHi = false;
                }
            }
            if (haveHi) {
                char buf[3] = { hi, '0', '\0' };
                outBytes.push_back(static_cast<uint8_t>(strtoul(buf, nullptr, 16)));
            }
            if (p < end && *p == '>') p++;
            return !outBytes.empty();
        };

        auto bytesToCode = [](const std::vector<uint8_t> &b) -> uint16_t {
            if (b.empty()) return 0;
            if (b.size() == 1) return b[0];
            return (static_cast<uint16_t>(b[0]) << 8) | b[1];
        };

        auto bytesToUnicodeVec = [](const std::vector<uint8_t> &b) -> std::vector<uint32_t> {
            std::vector<uint32_t> res;
            if (b.size() == 1) {
                if (b[0] > 0) res.push_back(b[0]);
            } else {
                for (size_t i = 0; i + 1 < b.size(); i += 2) {
                    uint32_t cp = (static_cast<uint32_t>(b[i]) << 8) | b[i + 1];
                    if (cp > 0) res.push_back(cp);
                }
            }
            return res;
        };

        // Check begincodespacerange for Type0 1-byte vs 2-byte code space hints
        bool hasOneByteRange = false;
        bool hasTwoByteRange = false;
        const char *csRange = strstr(p, "begincodespacerange");
        if (csRange) {
            const char *savedP = p;
            p = csRange + 19;
            while (p < end && strncmp(p, "endcodespacerange", 17) != 0) {
                std::vector<uint8_t> loB, hiB;
                if (!readHexBytes(loB) || !readHexBytes(hiB)) break;
                uint16_t hiCode = bytesToCode(hiB);
                if (loB.size() == 1 && hiB.size() == 1) {
                    hasOneByteRange = true;
                } else if (hiCode > 0x00FF) {
                    hasTwoByteRange = true;
                }
                skipPdfWhitespace(p, end);
            }
            p = savedP;
        }
        if ((outFont.subtype == "/Type0" || outFont.subtype == "Type0") && hasOneByteRange && !hasTwoByteRange) {
            outFont.isTwoByte = false;
        }

        while (p < end) {
            const char *bfchar = strstr(p, "beginbfchar");
            const char *bfrange = strstr(p, "beginbfrange");
            if (!bfchar && !bfrange) break;

            if (bfchar && (!bfrange || bfchar < bfrange)) {
                p = bfchar + 11;
                while (p < end && strncmp(p, "endbfchar", 9) != 0) {
                    std::vector<uint8_t> srcB, dstB;
                    if (!readHexBytes(srcB)) break;
                    if (!readHexBytes(dstB)) break;
                    // NOTE: Do NOT set outFont.isTwoByte = true here based on srcB.size() >= 2!
                    // Simple 1-byte fonts frequently write 4-hex-digit codes (<0020>) in /ToUnicode CMaps.
                    std::vector<uint32_t> uvec = bytesToUnicodeVec(dstB);
                    if (!uvec.empty()) {
                        outFont.toUnicode[bytesToCode(srcB)] = std::move(uvec);
                    }
                    skipPdfWhitespace(p, end);
                }
                if (p < end) p += 9;
            } else if (bfrange) {
                p = bfrange + 12;
                while (p < end && strncmp(p, "endbfrange", 10) != 0) {
                    skipPdfWhitespace(p, end);
                    if (strncmp(p, "endbfrange", 10) == 0) break;
                    std::vector<uint8_t> startB, endB;
                    if (!readHexBytes(startB)) break;
                    if (!readHexBytes(endB)) break;
                    uint16_t startCode = bytesToCode(startB);
                    uint16_t endCode = bytesToCode(endB);
                    skipPdfWhitespace(p, end);
                    if (p < end && *p == '[') {
                        p++;
                        bool closedBracket = false;
                        for (uint32_t c = startCode; c <= endCode && p < end; ++c) {
                            skipPdfWhitespace(p, end);
                            if (p < end && *p == ']') { p++; closedBracket = true; break; }
                            std::vector<uint8_t> dstB;
                            if (!readHexBytes(dstB)) break;
                            std::vector<uint32_t> uvec = bytesToUnicodeVec(dstB);
                            if (!uvec.empty()) {
                                outFont.toUnicode[static_cast<uint16_t>(c)] = std::move(uvec);
                            }
                        }
                        if (!closedBracket) {
                            while (p < end && *p != ']') p++;
                            if (p < end && *p == ']') p++;
                        }
                    } else {
                        std::vector<uint8_t> baseB;
                        if (!readHexBytes(baseB)) break;
                        std::vector<uint32_t> baseVec = bytesToUnicodeVec(baseB);
                        if (!baseVec.empty()) {
                            uint32_t span = std::min<uint32_t>(512, endCode >= startCode ? (endCode - startCode) : 0);
                            for (uint32_t i = 0; i <= span; ++i) {
                                std::vector<uint32_t> uvec = baseVec;
                                uvec.back() += i;
                                outFont.toUnicode[static_cast<uint16_t>(startCode + i)] = std::move(uvec);
                            }
                        }
                    }
                    skipPdfWhitespace(p, end);
                }
                if (p < end) p += 10;
            }
        }
    }

    void parseCidWidthsArray(const std::string &wArrStr, FontResource &outFont) {
        const char *p = wArrStr.c_str();
        const char *end = p + wArrStr.size();
        if (p < end && *p == '[') p++;

        while (p < end) {
            skipPdfWhitespace(p, end);
            if (p >= end || *p == ']') break;
            if (!std::isdigit(static_cast<unsigned char>(*p))) { p++; continue; }
            char *endp = nullptr;
            int firstCid = static_cast<int>(strtol(p, &endp, 10));
            p = endp;
            skipPdfWhitespace(p, end);
            if (p >= end) break;

            if (*p == '[') {
                p++;
                int cid = firstCid;
                while (p < end && *p != ']') {
                    skipPdfWhitespace(p, end);
                    if (p >= end || *p == ']') break;
                    if (std::isdigit(static_cast<unsigned char>(*p)) || *p == '-' || *p == '.') {
                        float wVal = strtof(p, &endp);
                        if (endp > p) {
                            outFont.cidWidths[static_cast<uint16_t>(cid++)] = static_cast<int>(wVal + 0.5f);
                            p = endp;
                        } else p++;
                    } else p++;
                }
                if (p < end && *p == ']') p++;
            } else if (std::isdigit(static_cast<unsigned char>(*p))) {
                int lastCid = static_cast<int>(strtol(p, &endp, 10));
                p = endp;
                skipPdfWhitespace(p, end);
                if (p < end && (std::isdigit(static_cast<unsigned char>(*p)) || *p == '-' || *p == '.')) {
                    float wVal = strtof(p, &endp);
                    p = endp;
                    int wInt = static_cast<int>(wVal + 0.5f);
                    int span = std::min(512, std::max(0, lastCid - firstCid));
                    for (int i = 0; i <= span; ++i) {
                        outFont.cidWidths[static_cast<uint16_t>(firstCid + i)] = wInt;
                    }
                }
            } else {
                p++;
            }
        }
    }

    void parseFontEncoding(const std::string &encVal, FontResource &outFont) {
        if (encVal.empty()) return;
        if (encVal.find("Identity-H") != std::string::npos || encVal.find("Identity-V") != std::string::npos) {
            outFont.isTwoByte = true;
            return;
        }
        if (encVal.find("MacRomanEncoding") != std::string::npos) {
            outFont.useMacRoman = true;
        }

        std::string encDict = encVal;
        int encRef = parseRefObjId(encVal);
        if (encRef > 0) {
            getObjectDict(encRef, encDict);
        }
        if (encDict.find("<<") == std::string::npos) return;

        std::string baseEnc = extractDictValue(encDict, "BaseEncoding");
        if (baseEnc.find("MacRomanEncoding") != std::string::npos) {
            outFont.useMacRoman = true;
        }

        std::string diffStr = extractDictValue(encDict, "Differences");
        int diffRef = parseRefObjId(diffStr);
        if (diffRef > 0) diffStr = getObjectContent(diffRef);
        if (diffStr.empty()) return;

        const char *p = diffStr.c_str();
        const char *end = p + diffStr.size();
        int curCode = 0;
        while (p < end) {
            skipPdfWhitespace(p, end);
            if (p >= end) break;
            if (std::isdigit(static_cast<unsigned char>(*p))) {
                char *endp = nullptr;
                curCode = static_cast<int>(strtol(p, &endp, 10));
                p = endp;
            } else if (*p == '/') {
                p++;
                const char *ns = p;
                while (p < end && !isPdfWhitespace(*p) && !isPdfDelimiter(*p)) p++;
                std::string gName(ns, p);
                if (!gName.empty() && curCode >= 0 && curCode < 256) {
                    outFont.encodingNames[static_cast<uint16_t>(curCode)] = gName;
                    uint32_t cp = pdfGlyphNameToUnicode(gName);
                    if (cp > 0) {
                        outFont.encodingDiffs[static_cast<uint16_t>(curCode)] = cp;
                    }
                }
                curCode++;
            } else {
                p++;
            }
        }
    }

    void loadFontDescriptorAndTTF(const std::string &fdVal, FontResource &outFont) {
        int fdRef = parseRefObjId(fdVal);
        std::string fdDict = fdVal;
        if (fdRef > 0) {
            if (!getObjectDict(fdRef, fdDict)) return;
        }
        if (fdDict.empty()) return;

        std::string mwStr = extractDictValue(fdDict, "MissingWidth");
        if (!mwStr.empty() && outFont.defaultWidth == 0) {
            outFont.defaultWidth = atoi(mwStr.c_str());
        }
        std::string itAngle = extractDictValue(fdDict, "ItalicAngle");
        if (!itAngle.empty() && std::fabs(strtof(itAngle.c_str(), nullptr)) > 1.0f) {
            outFont.isItalic = true;
        }
        std::string stemV = extractDictValue(fdDict, "StemV");
        if (!stemV.empty() && atoi(stemV.c_str()) >= 120) {
            outFont.isBold = true;
        }

        // Load embedded TrueType or CFF/Type1C font program (/FontFile2 or /FontFile3),
        // skipping Tesseract/MuPDF's invisible OCR placeholder font (/GlyphLessFont)
        int ff2Ref = 0;
        if (outFont.baseFont.find("GlyphLess") == std::string::npos) {
            ff2Ref = parseRefObjId(extractDictValue(fdDict, "FontFile2"));
            if (ff2Ref <= 0) {
                ff2Ref = parseRefObjId(extractDictValue(fdDict, "FontFile3"));
            }
        }
        if (ff2Ref > 0) {
            std::string ff2Dict;
            int64_t ff2StreamOff = -1;
            if (getObjectDict(ff2Ref, ff2Dict, &ff2StreamOff) && ff2StreamOff > 0) {
                std::vector<uint8_t> ttfBytes;
                if (readDecompressedStream(ff2Dict, ff2StreamOff, ttfBytes) && ttfBytes.size() >= 16) {
                    outFont.ttf.load(std::move(ttfBytes));
                }
            }
        }

        // For Type 1 fonts (/FontFile) without explicit encoding mappings, extract "dup <code> /<glyph> put"
        // from the ASCII header of the Type 1 font program (standard in TeX/dvips PDFs).
        int ff1Ref = parseRefObjId(extractDictValue(fdDict, "FontFile"));
        if (ff1Ref > 0 && outFont.toUnicode.empty() && outFont.encodingDiffs.empty()) {
            std::string ff1Dict;
            int64_t ff1StreamOff = -1;
            if (getObjectDict(ff1Ref, ff1Dict, &ff1StreamOff) && ff1StreamOff > 0) {
                std::vector<uint8_t> t1Bytes;
                if (readDecompressedStream(ff1Dict, ff1StreamOff, t1Bytes) && !t1Bytes.empty()) {
                    size_t scanLen = std::min<size_t>(t1Bytes.size(), 16384);
                    const char *p = reinterpret_cast<const char *>(t1Bytes.data());
                    const char *end = p + scanLen;
                    while (p + 10 < end) {
                        const char *dupPos = strstr(p, "dup ");
                        if (!dupPos || dupPos >= end) break;
                        p = dupPos + 4;
                        skipPdfWhitespace(p, end);
                        if (p < end && std::isdigit(static_cast<unsigned char>(*p))) {
                            char *endp = nullptr;
                            int code = static_cast<int>(strtol(p, &endp, 10));
                            p = endp;
                            skipPdfWhitespace(p, end);
                            if (p < end && *p == '/') {
                                p++;
                                const char *ns = p;
                                while (p < end && !isPdfWhitespace(*p) && !isPdfDelimiter(*p)) p++;
                                std::string gName(ns, p);
                                if (!gName.empty() && code >= 0 && code < 256) {
                                    outFont.encodingNames[static_cast<uint16_t>(code)] = gName;
                                    uint32_t cp = pdfGlyphNameToUnicode(gName);
                                    if (cp > 0) {
                                        outFont.encodingDiffs[static_cast<uint16_t>(code)] = cp;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    const FontResource &getOrLoadFont(int fontObjId, const std::string &inlineDict) {
        int key = (fontObjId > 0)
                      ? fontObjId
                      : -static_cast<int>((std::hash<std::string>{}(inlineDict) & 0x3FFFFFFF) + 1);
        auto it = fontObjCache.find(key);
        if (it != fontObjCache.end()) return it->second;

        FontResource font;
        std::string fDict = inlineDict;
        if (fontObjId > 0 && fDict.empty()) {
            getObjectDict(fontObjId, fDict);
        }

        font.subtype = extractDictValue(fDict, "Subtype");
        font.baseFont = extractDictValue(fDict, "BaseFont");
        if (!font.baseFont.empty() && font.baseFont[0] == '/') font.baseFont.erase(0, 1);
        if (font.baseFont.find("Bold") != std::string::npos || font.baseFont.find("Black") != std::string::npos) {
            font.isBold = true;
        }
        if (font.baseFont.find("Italic") != std::string::npos || font.baseFont.find("Oblique") != std::string::npos) {
            font.isItalic = true;
        }

        // Only composite /Type0 fonts default to 2-byte character codes
        if (font.subtype == "/Type0" || font.subtype == "Type0") {
            font.isTwoByte = true;
            font.defaultWidth = 1000;
        }

        parseFontEncoding(extractDictValue(fDict, "Encoding"), font);

        int tuRef = parseRefObjId(extractDictValue(fDict, "ToUnicode"));
        if (tuRef > 0) {
            std::string tuDict;
            int64_t tuStreamOff = -1;
            if (getObjectDict(tuRef, tuDict, &tuStreamOff) && tuStreamOff > 0) {
                std::vector<uint8_t> tuDec;
                if (readDecompressedStream(tuDict, tuStreamOff, tuDec) && !tuDec.empty()) {
                    std::string cmapStr(reinterpret_cast<const char *>(tuDec.data()), tuDec.size());
                    parseToUnicodeCMap(cmapStr, font);
                }
            }
        }

        if (font.subtype == "/Type0" || font.subtype == "Type0") {
            std::string descStr = extractDictValue(fDict, "DescendantFonts");
            std::string cidDict;
            int descRef = parseRefObjId(descStr);
            if (descRef > 0) {
                std::string dContent = getObjectContent(descRef);
                size_t dStart = dContent.find("<<");
                if (dStart != std::string::npos) {
                    size_t dEnd = findMatchingDictEnd(dContent.c_str(), dContent.size(), dStart);
                    cidDict = (dEnd != std::string::npos) ? dContent.substr(dStart, dEnd - dStart) : dContent.substr(dStart);
                } else {
                    std::vector<int> descIds = parseRefArray(dContent);
                    if (!descIds.empty()) getObjectDict(descIds[0], cidDict);
                }
            } else {
                // Check for inline dictionary inside array: /DescendantFonts [ << ... >> ]
                size_t dStart = descStr.find("<<");
                if (dStart != std::string::npos) {
                    size_t dEnd = findMatchingDictEnd(descStr.c_str(), descStr.size(), dStart);
                    cidDict = (dEnd != std::string::npos) ? descStr.substr(dStart, dEnd - dStart) : descStr.substr(dStart);
                } else {
                    std::vector<int> descIds = parseRefArray(descStr);
                    if (!descIds.empty()) getObjectDict(descIds[0], cidDict);
                }
            }

            if (!cidDict.empty()) {
                std::string dwStr = extractDictValue(cidDict, "DW");
                if (!dwStr.empty()) font.defaultWidth = atoi(dwStr.c_str());

                std::string wStr = extractDictValue(cidDict, "W");
                int wRef = parseRefObjId(wStr);
                if (wRef > 0) wStr = getObjectContent(wRef);
                if (!wStr.empty()) parseCidWidthsArray(wStr, font);

                std::string ctgStr = extractDictValue(cidDict, "CIDToGIDMap");
                int ctgRef = parseRefObjId(ctgStr);
                if (ctgRef > 0) {
                    std::string ctgDict;
                    int64_t ctgOff = -1;
                    if (getObjectDict(ctgRef, ctgDict, &ctgOff) && ctgOff > 0) {
                        std::vector<uint8_t> ctgDec;
                        if (readDecompressedStream(ctgDict, ctgOff, ctgDec) && ctgDec.size() >= 2) {
                            size_t nCids = ctgDec.size() / 2;
                            font.cidToGid.resize(nCids);
                            for (size_t i = 0; i < nCids; ++i) {
                                font.cidToGid[i] = (static_cast<uint16_t>(ctgDec[i * 2]) << 8) | ctgDec[i * 2 + 1];
                            }
                        }
                    }
                }

                loadFontDescriptorAndTTF(extractDictValue(cidDict, "FontDescriptor"), font);
            }
        } else {
            // Simple Font (Type1, TrueType, MMType1, Type3)
            std::string fcStr = extractDictValue(fDict, "FirstChar");
            if (!fcStr.empty()) font.firstChar = atoi(fcStr.c_str());

            std::string wStr = extractDictValue(fDict, "Widths");
            int wRef = parseRefObjId(wStr);
            if (wRef > 0) wStr = getObjectContent(wRef);
            if (!wStr.empty()) {
                std::vector<float> wFloats = parseFloatArray(wStr);
                font.widths.reserve(wFloats.size());
                for (float wf : wFloats) {
                    font.widths.push_back(static_cast<int>(wf + 0.5f));
                }
            }

            loadFontDescriptorAndTTF(extractDictValue(fDict, "FontDescriptor"), font);
        }

        if (fontObjCache.size() < 12) {
            logDiag("font[" + std::to_string(fontObjId) + "]: base=" +
                    (font.baseFont.empty() ? "none" : font.baseFont) +
                    " sub=" + font.subtype +
                    " 2b=" + std::to_string(font.isTwoByte ? 1 : 0) +
                    " ttf=" + std::to_string(font.ttf.isValid() ? 1 : 0) +
                    "(g=" + std::to_string(font.ttf.numGlyphs()) +
                    ",cmap=" + std::to_string(font.ttf.hasCmap() ? 1 : 0) + ")" +
                    " toUni=" + std::to_string(font.toUnicode.size()) +
                    " diff=" + std::to_string(font.encodingDiffs.size()));
        }

        auto res = fontObjCache.emplace(key, std::move(font));
        return res.first->second;
    }

    // Box-integrated anti-aliased Unifont fallback renderer (with bold/italic synthesis and proportional advance fit)
    static void drawUnifontCodepointSmooth(
        PdfCanvas &canvas,
        float dstX, float dstY,
        float targetHeightPx,
        float maxAdvancePx,
        uint8_t inkGray,
        int codepoint,
        bool isBold,
        bool isItalic)
    {
        if (codepoint <= 0 || codepoint > 0xFFFF) return;
        const uint8_t *unifontBase = FontHandler::start();
        const uint8_t *widthsBase = FontHandler::startCharWidths();
        if (!unifontBase || !widthsBase) return;

        int charCells = ((widthsBase[codepoint / 8] >> (codepoint % 8)) & 1) + 1;
        int fullW = charCells * 8;
        int srcH = 16;
        const uint8_t *glyph = unifontBase + (32 * codepoint);

        auto rawBit = [&](int sx, int sy) -> int {
            if (sx < 0 || sx >= fullW || sy < 0 || sy >= srcH) return 0;
            if (charCells == 1) {
                return (glyph[sy] >> (7 - sx)) & 1;
            }
            int byteIdx = sy * 2 + (sx >= 8 ? 1 : 0);
            return (glyph[byteIdx] >> (7 - (sx & 7))) & 1;
        };

        // Trim empty left/right columns of the fixed 8x16 cell so proportional/condensed PDF fonts
        // (such as narrow 'i', 'l', 't', '.', or condensed headlines) fit naturally within their advance width
        int minCol = fullW, maxCol = -1;
        for (int sy = 0; sy < srcH; ++sy) {
            for (int sx = 0; sx < fullW; ++sx) {
                if (rawBit(sx, sy)) {
                    if (sx < minCol) minCol = sx;
                    if (sx > maxCol) maxCol = sx;
                }
            }
        }
        if (maxCol < minCol) return; // Blank glyph
        int cropX0 = std::max(0, minCol - 1);
        int cropX1 = std::min(fullW - 1, maxCol + 1);
        int srcW = std::max(3, cropX1 - cropX0 + 1);

        auto sampleBit = [&](int sx, int sy) -> float {
            return rawBit(cropX0 + sx, sy) ? 1.0f : 0.0f;
        };

        // Scale smoothly down to 5.5px for small magazine captions while respecting proportional advance width
        float drawH = std::max(5.5f, std::min(96.0f, targetHeightPx));
        float drawW = drawH * (static_cast<float>(srcW) / 16.0f);
        if (maxAdvancePx > 1.2f) {
            drawW = std::min(drawW, maxAdvancePx * 0.96f);
        }
        drawW = std::max(2.0f, drawW);
        float topY = dstY - (drawH * 13.0f) / 16.0f;

        int y0 = static_cast<int>(std::floor(topY));
        int y1 = static_cast<int>(std::ceil(topY + drawH));
        int extraBold = isBold ? std::max(1, static_cast<int>(drawH / 16.0f + 0.5f)) : 0;

        // High-contrast stroke-preserving Unifont rasterizer:
        // Uses max-texel overlap within each destination pixel footprint so 1-texel-wide strokes at small
        // magazine sizes (5.5px-12px) never get diluted by box-averaging into light gray and thresholded away.
        float footprintX = std::max(1.0f, static_cast<float>(srcW) / drawW);
        float footprintY = std::max(1.0f, static_cast<float>(srcH) / drawH);
        const uint8_t haloGray = (inkGray < 128) ? 255 : 0;
        int clipX0, clipY0, clipX1, clipY1;
        canvas.getClipRect(clipX0, clipY0, clipX1, clipY1);

        struct HitPx { int x; int y; };
        std::vector<HitPx> hits;
        hits.reserve(static_cast<size_t>((y1 - y0 + 2) * (drawW + 3)));

        for (int py = y0; py <= y1; ++py) {
            float relY = (static_cast<float>(py) + 0.5f - topY) / drawH;
            if (relY < 0.0f || relY >= 1.0f) continue;
            float syCenter = relY * static_cast<float>(srcH);
            float sy0 = syCenter - 0.5f * footprintY;
            float sy1 = syCenter + 0.5f * footprintY;
            int iyStart = std::max(0, static_cast<int>(std::floor(sy0)));
            int iyEnd   = std::min(srcH - 1, static_cast<int>(std::floor(sy1)));

            float italicShift = isItalic ? ((0.8f - relY) * drawH * 0.20f) : 0.0f;
            float rowLeftX = dstX + italicShift;
            int x0 = static_cast<int>(std::floor(rowLeftX));
            int x1 = static_cast<int>(std::ceil(rowLeftX + drawW)) + extraBold;

            for (int px = x0; px <= x1; ++px) {
                float maxCov = 0.0f;
                for (int b = 0; b <= extraBold; ++b) {
                    float relX = (static_cast<float>(px - b) + 0.5f - rowLeftX) / drawW;
                    if (relX < 0.0f || relX >= 1.0f) continue;
                    float sxCenter = relX * static_cast<float>(srcW);
                    float sx0 = sxCenter - 0.5f * footprintX;
                    float sx1 = sxCenter + 0.5f * footprintX;
                    int ixStart = std::max(0, static_cast<int>(std::floor(sx0)));
                    int ixEnd   = std::min(srcW - 1, static_cast<int>(std::floor(sx1)));

                    for (int iy = iyStart; iy <= iyEnd; ++iy) {
                        float wy = (std::min(sy1, static_cast<float>(iy + 1)) - std::max(sy0, static_cast<float>(iy)))
                                   / std::min(1.0f, footprintY);
                        if (wy <= 0.0f) continue;
                        for (int ix = ixStart; ix <= ixEnd; ++ix) {
                            if (sampleBit(ix, iy) <= 0.0f) continue;
                            float wx = (std::min(sx1, static_cast<float>(ix + 1)) - std::max(sx0, static_cast<float>(ix)))
                                       / std::min(1.0f, footprintX);
                            if (wx <= 0.0f) continue;
                            maxCov = std::max(maxCov, wx * wy);
                        }
                    }
                }
                if (maxCov >= 0.20f) {
                    hits.push_back({ px, py });
                }
            }
        }

        // First pass: place a 1px contrast halo on mid-tone/textured image backgrounds (24 < dst < 232)
        for (const auto &hp : hits) {
            const int dx[4] = { -1, 1, 0, 0 };
            const int dy[4] = { 0, 0, -1, 1 };
            for (int k = 0; k < 4; ++k) {
                int nx = hp.x + dx[k];
                int ny = hp.y + dy[k];
                if (nx >= clipX0 && nx < clipX1 && ny >= clipY0 && ny < clipY1) {
                    uint8_t &ndst = canvas.buffer()[ny * canvas.width() + nx];
                    if (ndst > 24 && ndst < 232) {
                        ndst = haloGray;
                    }
                }
            }
        }
        // Second pass: write solid inkGray (alpha = 255) for all glyph stroke pixels
        for (const auto &hp : hits) {
            canvas.blendPixel(hp.x, hp.y, inkGray, 255);
        }
    }

    // Decodes a raw/filtered pixel buffer (1/2/4/8-bit Gray, RGB, CMYK, or Indexed palette) to 8-bit grayscale
    bool decodeRawPixelsToGrayBuffer(
        const std::string &imgDict,
        const std::vector<uint8_t> &pixelBytes,
        int w, int h, int bpc, int comps,
        uint8_t *&outGray, bool &outIsMask)
    {
        outGray = nullptr;
        outIsMask = (extractDictValue(imgDict, "ImageMask") == "true" || extractDictValue(imgDict, "IM") == "true");
        if (w <= 0 || h <= 0 || pixelBytes.empty()) return false;

        if (outIsMask) {
            bpc = 1;
            comps = 1;
        }

        // Check /Decode array for inverted polarity (e.g. [1 0])
        bool invertBits = false;
        std::string decodeStr = extractDictValue(imgDict, "Decode");
        if (decodeStr.empty()) decodeStr = extractDictValue(imgDict, "D");
        if (!decodeStr.empty()) {
            std::vector<float> decArr = parseFloatArray(decodeStr);
            if (decArr.size() >= 2 && decArr[0] > decArr[1]) {
                invertBits = true;
            }
        }

        // Check if /ColorSpace is [/Indexed base hival lookup]
        std::vector<uint8_t> paletteGray;
        std::string cs = extractDictValue(imgDict, "ColorSpace");
        if (cs.empty()) cs = extractDictValue(imgDict, "CS");
        int csRef = parseRefObjId(cs);
        if (csRef > 0) cs = getObjectContent(csRef);
        if (cs.find("Indexed") != std::string::npos || cs.find("/I") != std::string::npos) {
            comps = 1;
            int baseComps = (cs.find("CMYK") != std::string::npos) ? 4 : ((cs.find("Gray") != std::string::npos) ? 1 : 3);
            std::vector<uint8_t> palBytes;
            std::vector<int> refs = parseRefArray(cs);
            if (!refs.empty()) {
                std::string palDict;
                int64_t palOff = -1;
                if (getObjectDict(refs.back(), palDict, &palOff) && palOff > 0) {
                    readDecompressedStream(palDict, palOff, palBytes);
                }
            }
            if (palBytes.empty()) {
                size_t hexPos = cs.find('<');
                size_t strPos = cs.find('(');
                if (strPos != std::string::npos) {
                    std::string s = decodePdfString(cs.substr(strPos, cs.rfind(')') - strPos + 1));
                    palBytes.assign(s.begin(), s.end());
                } else if (hexPos != std::string::npos && cs.rfind('>') > hexPos) {
                    std::string s = decodePdfString(cs.substr(hexPos, cs.rfind('>') - hexPos + 1));
                    palBytes.assign(s.begin(), s.end());
                }
            }
            if (!palBytes.empty() && baseComps > 0) {
                size_t nEntries = palBytes.size() / static_cast<size_t>(baseComps);
                paletteGray.resize(nEntries);
                for (size_t i = 0; i < nEntries; ++i) {
                    if (baseComps == 1) {
                        paletteGray[i] = palBytes[i];
                    } else if (baseComps == 3) {
                        uint8_t r = palBytes[i * 3], g = palBytes[i * 3 + 1], b = palBytes[i * 3 + 2];
                        paletteGray[i] = static_cast<uint8_t>((r * 77 + g * 150 + b * 29) >> 8);
                    } else {
                        uint8_t c = palBytes[i * 4], m = palBytes[i * 4 + 1], y = palBytes[i * 4 + 2], k = palBytes[i * 4 + 3];
                        uint8_t r = 255 - std::min(255, (c * (255 - k)) / 255 + k);
                        uint8_t g = 255 - std::min(255, (m * (255 - k)) / 255 + k);
                        uint8_t b = 255 - std::min(255, (y * (255 - k)) / 255 + k);
                        paletteGray[i] = static_cast<uint8_t>((r * 77 + g * 150 + b * 29) >> 8);
                    }
                }
            }
        }

        size_t pixels = static_cast<size_t>(w) * h;
        uint8_t *grayBuf = allocComicPageBuffer(pixels);
        if (!grayBuf) return false;

        if (bpc == 1) {
            int stride = (w + 7) / 8;
            for (int y = 0; y < h; ++y) {
                const uint8_t *row = pixelBytes.data() + static_cast<size_t>(y) * stride;
                uint8_t *dstRow = grayBuf + static_cast<size_t>(y) * w;
                if (static_cast<size_t>(y + 1) * stride > pixelBytes.size()) {
                    memset(dstRow, 255, static_cast<size_t>(w));
                    continue;
                }
                for (int x = 0; x < w; ++x) {
                    int bit = (row[x / 8] >> (7 - (x % 8))) & 1;
                    if (invertBits) bit ^= 1;
                    if (!paletteGray.empty() && static_cast<size_t>(bit) < paletteGray.size()) {
                        dstRow[x] = paletteGray[bit];
                    } else {
                        dstRow[x] = bit ? 255 : 0;
                    }
                }
            }
        } else if (bpc == 2 || bpc == 4) {
            int pixPerByte = 8 / bpc;
            int mask = (1 << bpc) - 1;
            int maxVal = mask;
            int stride = (w * bpc + 7) / 8;
            for (int y = 0; y < h; ++y) {
                const uint8_t *row = pixelBytes.data() + static_cast<size_t>(y) * stride;
                uint8_t *dstRow = grayBuf + static_cast<size_t>(y) * w;
                if (static_cast<size_t>(y + 1) * stride > pixelBytes.size()) {
                    memset(dstRow, 255, static_cast<size_t>(w));
                    continue;
                }
                for (int x = 0; x < w; ++x) {
                    int shift = (pixPerByte - 1 - (x % pixPerByte)) * bpc;
                    int val = (row[x / pixPerByte] >> shift) & mask;
                    if (!paletteGray.empty() && static_cast<size_t>(val) < paletteGray.size()) {
                        dstRow[x] = paletteGray[val];
                    } else {
                        dstRow[x] = static_cast<uint8_t>((val * 255) / maxVal);
                    }
                }
            }
        } else if (comps == 1 && bpc == 8) {
            size_t toCopy = std::min(pixels, pixelBytes.size());
            if (!paletteGray.empty()) {
                for (size_t i = 0; i < toCopy; ++i) {
                    uint8_t idx = pixelBytes[i];
                    grayBuf[i] = (idx < paletteGray.size()) ? paletteGray[idx] : idx;
                }
            } else {
                for (size_t i = 0; i < toCopy; ++i) {
                    grayBuf[i] = invertBits ? (255 - pixelBytes[i]) : pixelBytes[i];
                }
            }
            if (toCopy < pixels) memset(grayBuf + toCopy, 255, pixels - toCopy);
        } else if (comps == 3 && bpc == 8) {
            size_t maxPx = std::min(pixels, pixelBytes.size() / 3);
            for (size_t i = 0; i < maxPx; ++i) {
                uint8_t r = pixelBytes[i * 3], g = pixelBytes[i * 3 + 1], b = pixelBytes[i * 3 + 2];
                grayBuf[i] = static_cast<uint8_t>((r * 77 + g * 150 + b * 29) >> 8);
            }
            if (maxPx < pixels) memset(grayBuf + maxPx, 255, pixels - maxPx);
        } else if (comps == 4 && bpc == 8) {
            size_t maxPx = std::min(pixels, pixelBytes.size() / 4);
            for (size_t i = 0; i < maxPx; ++i) {
                uint8_t c = pixelBytes[i * 4], m = pixelBytes[i * 4 + 1], y = pixelBytes[i * 4 + 2], k = pixelBytes[i * 4 + 3];
                uint8_t r = 255 - std::min(255, (c * (255 - k)) / 255 + k);
                uint8_t g = 255 - std::min(255, (m * (255 - k)) / 255 + k);
                uint8_t b = 255 - std::min(255, (y * (255 - k)) / 255 + k);
                grayBuf[i] = static_cast<uint8_t>((r * 77 + g * 150 + b * 29) >> 8);
            }
            if (maxPx < pixels) memset(grayBuf + maxPx, 255, pixels - maxPx);
        } else {
            free(grayBuf);
            return false;
        }

        outGray = grayBuf;
        return true;
    }

    // Decodes an indirect /Subtype /Image XObject (JPEG, FlateJpeg, PNG, JBIG2, or Flate/LZW/A85 raw) to 8-bit grayscale
    bool decodeImageObjectToGray(int imgObjId, uint8_t *&outGray, int &outW, int &outH, bool &outIsMask, int depth = 0) {
        outGray = nullptr;
        outW = outH = 0;
        outIsMask = false;

        std::string imgDict;
        getObjectDict(imgObjId, imgDict);

        PageEntry imgEntry;
        if (!parseImageObj(imgObjId, 0, imgEntry)) {
            // Mixed Raster Content (MRC) fallback: if the primary image is an unsupported /JPXDecode
            // foreground layer with a decodable /SMask (such as a /JBIG2Decode or /FlateDecode bi-level
            // foreground text/illustration mask in scanned books like adults.pdf), decode the /SMask
            // and render its opaque foreground pixels as black ink!
            if (depth == 0 && !imgDict.empty()) {
                int smaskRef = parseRefObjId(extractDictValue(imgDict, "SMask"));
                if (smaskRef > 0) {
                    bool dummyMask = false;
                    if (decodeImageObjectToGray(smaskRef, outGray, outW, outH, dummyMask, depth + 1) &&
                        outGray && outW > 0 && outH > 0)
                    {
                        size_t totalPx = static_cast<size_t>(outW) * outH;
                        // In a PDF /SMask, sample 255 = opaque foreground ink and 0 = transparent background,
                        // whereas in a JBIG2 image bit 1 (ink) was mapped to 0 (black) and bit 0 (paper) to 255 (white).
                        // Ensure the majority background is white (255 = transparent in stencil mask) and minority foreground is black (0 = ink).
                        uint64_t sumGray = 0;
                        size_t step = std::max<size_t>(1, totalPx / 4096);
                        size_t samples = 0;
                        for (size_t i = 0; i < totalPx; i += step) {
                            sumGray += outGray[i];
                            samples++;
                        }
                        if (samples > 0 && (sumGray / samples) < 128) {
                            for (size_t i = 0; i < totalPx; ++i) {
                                outGray[i] = 255 - outGray[i];
                            }
                        }
                        // Boost foreground stroke contrast after normalizing polarity (255=paper, 0=ink)
                        for (size_t i = 0; i < totalPx; ++i) {
                            int ink = 255 - outGray[i];
                            outGray[i] = static_cast<uint8_t>(std::max(0, 255 - ink * 2));
                        }
                        outIsMask = true;
                        return true;
                    }
                }
            }
            return false;
        }

        bool decodedOk = false;
        if (imgEntry.format == ImageFormat::Jpeg || imgEntry.format == ImageFormat::FlateJpeg || imgEntry.format == ImageFormat::Png) {
            uint8_t *rawBytes = allocComicPageBuffer(imgEntry.streamLen + 4);
            if (!rawBytes) return false;
            if (!readBytesAt(imgEntry.streamOffset, rawBytes, imgEntry.streamLen)) {
                free(rawBytes);
                return false;
            }
            size_t n = imgEntry.streamLen;
            if (imgEntry.format == ImageFormat::FlateJpeg) {
                std::vector<uint8_t> dec;
                bool fOk = applyFilterChain(imgDict, rawBytes, n, dec);
                free(rawBytes);
                if (!fOk || dec.empty()) return false;
                rawBytes = allocComicPageBuffer(dec.size() + 4);
                if (!rawBytes) return false;
                memcpy(rawBytes, dec.data(), dec.size());
                n = dec.size();
            }
            if (imgEntry.format != ImageFormat::Png) {
                size_t offset = 0;
                while (offset + 1 < n && (rawBytes[offset] != 0xFF || rawBytes[offset + 1] != 0xD8)) offset++;
                if (offset > 0 && offset + 2 < n) {
                    memmove(rawBytes, rawBytes + offset, n - offset);
                    n -= offset;
                }
            }
            ComicZoomPageCache tmpCache;
            const char *extName = (imgEntry.format == ImageFormat::Png) ? "img.png" : "img.jpg";
            if (!decodeComicPageToZoomCache(rawBytes, n, extName, 0, tmpCache) || !tmpCache.grayBuf) {
                return false;
            }
            outGray = tmpCache.grayBuf;
            outW = tmpCache.width;
            outH = tmpCache.height;
            tmpCache.grayBuf = nullptr; // transfer ownership
            decodedOk = true;
        } else if (imgEntry.format == ImageFormat::Jbig2) {
            uint8_t *rawBytes = allocComicPageBuffer(imgEntry.streamLen);
            if (!rawBytes) return false;
            if (!readBytesAt(imgEntry.streamOffset, rawBytes, imgEntry.streamLen)) {
                free(rawBytes);
                return false;
            }
            bool invertBits = false;
            std::string decodeStr = extractDictValue(imgDict, "Decode");
            if (decodeStr.empty()) decodeStr = extractDictValue(imgDict, "D");
            if (!decodeStr.empty()) {
                std::vector<float> decArr = parseFloatArray(decodeStr);
                if (decArr.size() >= 2 && decArr[0] > decArr[1]) {
                    invertBits = true;
                }
            }
            outIsMask = (extractDictValue(imgDict, "ImageMask") == "true" || extractDictValue(imgDict, "IM") == "true");
            decodedOk = decodePdfJbig2ToGray(rawBytes, imgEntry.streamLen, imgEntry.width, imgEntry.height,
                                             invertBits, outGray, outW, outH);
            free(rawBytes);
        } else {
            // Check if this is a single /FlateDecode stream that can be streamed row-by-row
            // directly into an 8-bit grayscale buffer without allocating a multi-MB RGB buffer
            std::string filter = extractDictValue(imgDict, "Filter");
            if (filter.empty()) filter = extractDictValue(imgDict, "F");
            int filterRef = parseRefObjId(filter);
            if (filterRef > 0) filter = getObjectContent(filterRef);
            bool isSingleFlate = (filter == "/FlateDecode" || filter == "FlateDecode" || filter == "/Fl" || filter == "Fl");

            if (isSingleFlate && imgEntry.width > 0 && imgEntry.height > 0 && imgEntry.streamLen > 0) {
                outIsMask = (extractDictValue(imgDict, "ImageMask") == "true" || extractDictValue(imgDict, "IM") == "true");
                int bpc = outIsMask ? 1 : imgEntry.bitsPerComponent;
                int comps = outIsMask ? 1 : imgEntry.colorComponents;
                bool invertBits = false;
                std::string decodeStr = extractDictValue(imgDict, "Decode");
                if (decodeStr.empty()) decodeStr = extractDictValue(imgDict, "D");
                if (!decodeStr.empty()) {
                    std::vector<float> decArr = parseFloatArray(decodeStr);
                    if (decArr.size() >= 2 && decArr[0] > decArr[1]) invertBits = true;
                }

                std::vector<uint8_t> paletteGray;
                std::string cs = extractDictValue(imgDict, "ColorSpace");
                if (cs.empty()) cs = extractDictValue(imgDict, "CS");
                int csRef = parseRefObjId(cs);
                if (csRef > 0) cs = getObjectContent(csRef);
                if (cs.find("Indexed") != std::string::npos || cs.find("/I") != std::string::npos) {
                    comps = 1;
                    int baseComps = (cs.find("CMYK") != std::string::npos) ? 4 : ((cs.find("Gray") != std::string::npos) ? 1 : 3);
                    std::vector<uint8_t> palBytes;
                    std::vector<int> refs = parseRefArray(cs);
                    if (!refs.empty()) {
                        std::string palDict;
                        int64_t palOff = -1;
                        if (getObjectDict(refs.back(), palDict, &palOff) && palOff > 0) {
                            readDecompressedStream(palDict, palOff, palBytes);
                        }
                    }
                    if (palBytes.empty()) {
                        size_t hexPos = cs.find('<');
                        size_t strPos = cs.find('(');
                        if (strPos != std::string::npos) {
                            std::string s = decodePdfString(cs.substr(strPos, cs.rfind(')') - strPos + 1));
                            palBytes.assign(s.begin(), s.end());
                        } else if (hexPos != std::string::npos && cs.rfind('>') > hexPos) {
                            std::string s = decodePdfString(cs.substr(hexPos, cs.rfind('>') - hexPos + 1));
                            palBytes.assign(s.begin(), s.end());
                        }
                    }
                    if (!palBytes.empty() && baseComps > 0) {
                        size_t nEntries = palBytes.size() / static_cast<size_t>(baseComps);
                        paletteGray.resize(nEntries);
                        for (size_t i = 0; i < nEntries; ++i) {
                            if (baseComps == 1) paletteGray[i] = palBytes[i];
                            else if (baseComps == 3) {
                                uint8_t r = palBytes[i * 3], g = palBytes[i * 3 + 1], b = palBytes[i * 3 + 2];
                                paletteGray[i] = static_cast<uint8_t>((r * 77 + g * 150 + b * 29) >> 8);
                            } else {
                                uint8_t c = palBytes[i * 4], m = palBytes[i * 4 + 1], y = palBytes[i * 4 + 2], k = palBytes[i * 4 + 3];
                                uint8_t r = 255 - std::min(255, (c * (255 - k)) / 255 + k);
                                uint8_t g = 255 - std::min(255, (m * (255 - k)) / 255 + k);
                                uint8_t b = 255 - std::min(255, (y * (255 - k)) / 255 + k);
                                paletteGray[i] = static_cast<uint8_t>((r * 77 + g * 150 + b * 29) >> 8);
                            }
                        }
                    }
                }

                uint8_t *rawBytes = allocComicPageBuffer(imgEntry.streamLen);
                if (rawBytes) {
                    if (readBytesAt(imgEntry.streamOffset, rawBytes, imgEntry.streamLen)) {
                        decodedOk = inflatePdfFlateImageToGray(
                            rawBytes, imgEntry.streamLen, imgEntry.width, imgEntry.height,
                            bpc, comps, imgEntry.predictor, imgEntry.predictorColumns,
                            invertBits, paletteGray, outGray, outW, outH);
                    }
                    free(rawBytes);
                }
            }

            if (!decodedOk) {
                // Multi-filter / ASCII85 / LZW / RunLength / uncompressed bitmap
                std::vector<uint8_t> dec;
                if (!readDecompressedStream(imgDict, imgEntry.streamOffset, dec) || dec.empty()) {
                    return false;
                }
                outW = imgEntry.width;
                outH = imgEntry.height;
                decodedOk = decodeRawPixelsToGrayBuffer(imgDict, dec, imgEntry.width, imgEntry.height,
                                                        imgEntry.bitsPerComponent, imgEntry.colorComponents,
                                                        outGray, outIsMask);
            }
        }

        // Apply /SMask (soft-mask alpha channel) if present so transparent areas become white (255)
        // instead of rendering unmasked black RGB (0,0,0) artefacts!
        if (decodedOk && depth == 0 && outGray && outW > 0 && outH > 0) {
            int smaskRef = parseRefObjId(extractDictValue(imgDict, "SMask"));
            if (smaskRef > 0) {
                uint8_t *maskGray = nullptr;
                int mw = 0, mh = 0;
                bool dummyMask = false;
                if (decodeImageObjectToGray(smaskRef, maskGray, mw, mh, dummyMask, depth + 1) &&
                    maskGray && mw > 0 && mh > 0)
                {
                    for (int y = 0; y < outH; ++y) {
                        int my = std::min(mh - 1, (y * mh) / outH);
                        const uint8_t *mRow = maskGray + static_cast<size_t>(my) * mw;
                        uint8_t *dstRow = outGray + static_cast<size_t>(y) * outW;
                        for (int x = 0; x < outW; ++x) {
                            int mx = std::min(mw - 1, (x * mw) / outW);
                            int alpha = mRow[mx];
                            dstRow[x] = static_cast<uint8_t>((static_cast<int>(dstRow[x]) * alpha + 255 * (255 - alpha)) / 255);
                        }
                    }
                    free(maskGray);
                }
            }
        }
        return decodedOk;
    }

    // Parses a resource sub-dictionary (such as /Font or /XObject) into name -> (objId, inlineDict)
    struct ResEntry {
        int objId = -1;
        std::string inlineDict;
    };

    void parseResourceSubDict(
        const std::string &resDict,
        const char *subKey,
        std::unordered_map<std::string, ResEntry> &outMap)
    {
        if (resDict.empty()) return;
        std::string subStr = extractDictValue(resDict, subKey);
        int subRef = parseRefObjId(subStr);
        if (subRef > 0) getObjectDict(subRef, subStr);
        if (subStr.empty()) return;

        const char *p = subStr.c_str();
        const char *end = p + subStr.size();
        if (p + 1 < end && p[0] == '<' && p[1] == '<') p += 2;

        while (p < end) {
            skipPdfWhitespace(p, end);
            if (p >= end || (*p == '>' && p + 1 < end && *(p + 1) == '>')) break;
            if (*p != '/') { p++; continue; }
            p++;
            const char *ns = p;
            while (p < end && !isPdfWhitespace(*p) && !isPdfDelimiter(*p)) p++;
            std::string name(ns, p);
            skipPdfWhitespace(p, end);
            if (p >= end) break;

            if (*p == '<' && p + 1 < end && *(p + 1) == '<') {
                size_t dStart = static_cast<size_t>(p - subStr.c_str());
                size_t dEnd = findMatchingDictEnd(subStr.c_str(), subStr.size(), dStart);
                if (dEnd == std::string::npos) break;
                outMap[name] = { -1, subStr.substr(dStart, dEnd - dStart) };
                p = subStr.c_str() + dEnd;
            } else if (std::isdigit(static_cast<unsigned char>(*p))) {
                char *endp = nullptr;
                int oid = static_cast<int>(strtol(p, &endp, 10));
                p = endp;
                skipPdfWhitespace(p, end);
                if (p < end && std::isdigit(static_cast<unsigned char>(*p))) {
                    strtol(p, &endp, 10);
                    p = endp;
                    skipPdfWhitespace(p, end);
                    if (p < end && *p == 'R') p++;
                }
                if (oid > 0) {
                    outMap[name] = { oid, "" };
                }
            } else {
                while (p < end && !isPdfWhitespace(*p) && !isPdfDelimiter(*p)) p++;
            }
        }
    }

    struct GState {
        float ctm[6] = { 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
        uint8_t strokeGray = 0;
        uint8_t fillGray = 0;
        float strokeAlpha = 1.0f;
        float fillAlpha = 1.0f;
        bool hasSoftMask = false;
        float lineWidth = 1.0f;
        std::vector<float> dashArray;
        float dashPhase = 0.0f;
        std::string fontName;
        float fontSize = 12.0f;
        float charSpace = 0.0f;
        float wordSpace = 0.0f;
        float horizScale = 1.0f;
        float leading = 14.0f;
        float textRise = 0.0f;
        int renderMode = 0; // 0=fill, 1=stroke, 2=fill+stroke, 3=invisible
        int clipX0 = 0, clipY0 = 0, clipX1 = 0, clipY1 = 0;
    };

    static void multiplyMatrix(const float a[6], const float b[6], float out[6]) {
        float r[6] = {
            a[0] * b[0] + a[1] * b[2],
            a[0] * b[1] + a[1] * b[3],
            a[2] * b[0] + a[3] * b[2],
            a[2] * b[1] + a[3] * b[3],
            a[4] * b[0] + a[5] * b[2] + b[4],
            a[4] * b[1] + a[5] * b[3] + b[5]
        };
        memcpy(out, r, sizeof(r));
    }

    struct StreamStats {
        int btCount = 0;
        int tfCount = 0;
        int tjCount = 0;
        int doCount = 0;
        int pathCount = 0;
        int glyphsTtf = 0;
        int glyphsUnifont = 0;
        int missingFonts = 0;
    };

    void executeContentStream(
        const std::string &streamData,
        const std::string &resourcesDict,
        const float userToCanvas[6],
        GState &gs,
        PdfCanvas &canvas,
        int formDepth,
        StreamStats &stats,
        const std::unordered_map<std::string, ResEntry> *parentFontRes = nullptr,
        const std::unordered_map<std::string, ResEntry> *parentXObjRes = nullptr,
        const std::unordered_map<std::string, ResEntry> *parentExtGsRes = nullptr)
    {
        if (streamData.empty() || formDepth > 5) return;

        std::unordered_map<std::string, ResEntry> fontResMap;
        std::unordered_map<std::string, ResEntry> xobjResMap;
        std::unordered_map<std::string, ResEntry> extGsResMap;
        if (parentFontRes) fontResMap = *parentFontRes;
        if (parentXObjRes) xobjResMap = *parentXObjRes;
        if (parentExtGsRes) extGsResMap = *parentExtGsRes;
        parseResourceSubDict(resourcesDict, "Font", fontResMap);
        parseResourceSubDict(resourcesDict, "XObject", xobjResMap);
        parseResourceSubDict(resourcesDict, "ExtGState", extGsResMap);

        std::vector<GState> gsStack;
        float tm[6] = { 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
        float tlm[6] = { 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
        const float canvasScale = std::hypot(userToCanvas[0], userToCanvas[1]);

        auto mapPointToCanvas = [&](float ux, float uy) -> PdfCanvas::Point {
            float gx = gs.ctm[0] * ux + gs.ctm[2] * uy + gs.ctm[4];
            float gy = gs.ctm[1] * ux + gs.ctm[3] * uy + gs.ctm[5];
            return {
                userToCanvas[0] * gx + userToCanvas[2] * gy + userToCanvas[4],
                userToCanvas[1] * gx + userToCanvas[3] * gy + userToCanvas[5]
            };
        };

        // Check if all XObjects on the page are undecodable pure JPXDecode without an SMask;
        // if so, allow 3 Tr (invisible OCR text) to render as visible fallback text!
        bool hasUndecodableJpxOnly = false;
        if (!xobjResMap.empty()) {
            bool anyJpxNoMask = false;
            bool anyDecodableXObj = false;
            for (const auto &kv : xobjResMap) {
                if (kv.second.objId <= 0) continue;
                std::string xd;
                if (!getObjectDict(kv.second.objId, xd)) continue;
                std::string flt = extractDictValue(xd, "Filter");
                if (flt.find("JPXDecode") != std::string::npos) {
                    if (parseRefObjId(extractDictValue(xd, "SMask")) > 0) {
                        anyDecodableXObj = true;
                    } else {
                        anyJpxNoMask = true;
                    }
                } else {
                    anyDecodableXObj = true;
                }
            }
            hasUndecodableJpxOnly = (anyJpxNoMask && !anyDecodableXObj);
        }

        auto renderTextToken = [&](const std::string &rawToken) {
            if (rawToken.size() < 2 || (gs.renderMode == 3 && !hasUndecodableJpxOnly)) return;
            stats.tjCount++;

            const FontResource *font = nullptr;
            auto fIt = fontResMap.find(gs.fontName);
            if (fIt == fontResMap.end() && fontResMap.size() == 1) {
                fIt = fontResMap.begin();
            }
            if (fIt != fontResMap.end()) {
                font = &getOrLoadFont(fIt->second.objId, fIt->second.inlineDict);
            } else {
                stats.missingFonts++;
            }

            // Extract raw bytes from (...) or <...>
            std::string rawBytes;
            if (rawToken.front() == '(' && rawToken.back() == ')') {
                std::string inner = rawToken.substr(1, rawToken.size() - 2);
                for (size_t i = 0; i < inner.size(); ++i) {
                    if (inner[i] == '\\' && i + 1 < inner.size()) {
                        char nx = inner[++i];
                        if (nx == 'n') rawBytes += '\n';
                        else if (nx == 'r') rawBytes += '\r';
                        else if (nx == 't') rawBytes += '\t';
                        else if (nx == 'b') rawBytes += '\b';
                        else if (nx == 'f') rawBytes += '\f';
                        else if (nx == '\r') {
                            if (i + 1 < inner.size() && inner[i + 1] == '\n') ++i;
                        } else if (nx == '\n') {
                            // line continuation
                        } else if (nx >= '0' && nx <= '7') {
                            int oct = nx - '0';
                            if (i + 1 < inner.size() && inner[i + 1] >= '0' && inner[i + 1] <= '7') {
                                oct = (oct << 3) + (inner[++i] - '0');
                                if (i + 1 < inner.size() && inner[i + 1] >= '0' && inner[i + 1] <= '7') {
                                    oct = (oct << 3) + (inner[++i] - '0');
                                }
                            }
                            rawBytes += static_cast<char>(oct);
                        } else {
                            rawBytes += nx;
                        }
                    } else {
                        rawBytes += inner[i];
                    }
                }
            } else if (rawToken.front() == '<' && rawToken.back() == '>') {
                std::string hex = rawToken.substr(1, rawToken.size() - 2);
                char hi = 0;
                bool haveHi = false;
                for (char c : hex) {
                    if (isPdfWhitespace(c)) continue;
                    if (!haveHi) { hi = c; haveHi = true; }
                    else {
                        char b[3] = { hi, c, '\0' };
                        rawBytes += static_cast<char>(strtol(b, nullptr, 16));
                        haveHi = false;
                    }
                }
                if (haveHi) {
                    char b[3] = { hi, '0', '\0' };
                    rawBytes += static_cast<char>(strtol(b, nullptr, 16));
                }
            }

            // Decode character codes (1-byte or 2-byte CID)
            std::vector<uint16_t> charCodes;
            bool twoByte = (font && font->isTwoByte) ||
                           (rawBytes.size() >= 2 && static_cast<uint8_t>(rawBytes[0]) == 0xFE && static_cast<uint8_t>(rawBytes[1]) == 0xFF);
            size_t startByte = (rawBytes.size() >= 2 && static_cast<uint8_t>(rawBytes[0]) == 0xFE && static_cast<uint8_t>(rawBytes[1]) == 0xFF) ? 2 : 0;

            if (twoByte && rawBytes.size() >= startByte + 2) {
                for (size_t i = startByte; i + 1 < rawBytes.size(); i += 2) {
                    uint16_t cid = (static_cast<uint16_t>(static_cast<uint8_t>(rawBytes[i])) << 8) |
                                   static_cast<uint8_t>(rawBytes[i + 1]);
                    charCodes.push_back(cid);
                }
            } else {
                for (unsigned char b : rawBytes) {
                    charCodes.push_back(static_cast<uint16_t>(b));
                }
            }

            uint8_t inkGray = (gs.renderMode == 1) ? gs.strokeGray : gs.fillGray;
            // Contrast safeguard for 1-bit e-ink:
            // - If the author requested white/light text (inkGray >= 200), keep it crisp white (255)
            //   whenever the surrounding 9x9 region is not pure white paper (avgBg < 242). This prevents
            //   adjacent white letters in kerned TJ arrays or bright photo highlights from flipping a
            //   white letter to black!
            // - Otherwise (colored, gray, or black text), render with solid black ink (0) for maximum readability.
            {
                PdfCanvas::Point basePt = mapPointToCanvas(tm[4], tm[5]);
                int bx = static_cast<int>(basePt.x);
                int by = static_cast<int>(basePt.y);
                if (inkGray >= 200) {
                    int sumBg = 0, cntBg = 0;
                    for (int dy = -4; dy <= 4; dy += 2) {
                        int sy = by + dy;
                        if (sy < 0 || sy >= canvas.height()) continue;
                        for (int dx = -4; dx <= 4; dx += 2) {
                            int sx = bx + dx;
                            if (sx < 0 || sx >= canvas.width()) continue;
                            sumBg += canvas.buffer()[sy * canvas.width() + sx];
                            cntBg++;
                        }
                    }
                    int avgBg = (cntBg > 0) ? (sumBg / cntBg) : 255;
                    inkGray = (avgBg < 242) ? 255 : 0;
                } else {
                    inkGray = 0;
                }
            }

            // Compose text-to-canvas matrix: userToCanvas * gs.ctm * tm
            float ctmTm[6], textToCanvas[6];
            multiplyMatrix(tm, gs.ctm, ctmTm);
            multiplyMatrix(ctmTm, userToCanvas, textToCanvas);
            float effFontPx = std::fabs(gs.fontSize * std::hypot(textToCanvas[2], textToCanvas[3]));
            if (effFontPx < 1.0f) {
                effFontPx = std::fabs(gs.fontSize * std::hypot(textToCanvas[0], textToCanvas[1]));
            }
            const float xUnitToCanvasPx = std::hypot(textToCanvas[0], textToCanvas[1]);

            for (uint16_t cid : charCodes) {
                // 1. Resolve Unicode codepoints for this character code
                std::vector<uint32_t> unicodes;
                if (font) {
                    auto tuIt = font->toUnicode.find(cid);
                    if (tuIt != font->toUnicode.end()) {
                        unicodes = tuIt->second;
                    } else {
                        auto dfIt = font->encodingDiffs.find(cid);
                        if (dfIt != font->encodingDiffs.end()) {
                            unicodes.push_back(dfIt->second);
                        } else if (!font->isTwoByte && cid < 256) {
                            unicodes.push_back(font->useMacRoman
                                ? pdfMacRomanToUnicode(static_cast<uint8_t>(cid))
                                : pdfWinAnsiToUnicode(static_cast<uint8_t>(cid)));
                        } else {
                            unicodes.push_back(cid);
                        }
                    }
                } else {
                    unicodes.push_back(cid < 256 ? pdfWinAnsiToUnicode(static_cast<uint8_t>(cid)) : cid);
                }

                // Never rasterize whitespace codepoints (prevents empty space glyf from falling back to a wrong subset GID!)
                bool isWhitespace = (!unicodes.empty() && unicodes.size() == 1 &&
                                     (unicodes[0] <= 0x20 || unicodes[0] == 0xA0 || unicodes[0] == 0xFEFF)) ||
                                    (unicodes.empty() && cid <= 0x20);

                // 2. Render glyph via embedded TrueType (/FontFile2) or CFF (/FontFile3) if available
                bool renderedByTtf = false;
                int ttfGid = 0;
                if (!isWhitespace && font && font->ttf.isValid()) {
                    int candGids[6] = { 0, 0, 0, 0, 0, 0 };
                    int nCands = 0;
                    auto pushCand = [&](int g) {
                        if (g <= 0 || g >= font->ttf.numGlyphs()) return;
                        for (int k = 0; k < nCands; ++k) {
                            if (candGids[k] == g) return;
                        }
                        if (nCands < 6) candGids[nCands++] = g;
                    };

                    // First check explicit /Encoding /Differences glyph name if present (critical for Type1C/CFF subsets)
                    auto enIt = font->encodingNames.find(cid);
                    if (enIt != font->encodingNames.end()) {
                        pushCand(font->ttf.findGlyphByName(enIt->second));
                    }

                    if (font->isTwoByte) {
                        if (cid < font->cidToGid.size()) pushCand(font->cidToGid[cid]);
                        pushCand(font->ttf.findGlyphByCharCode(cid));
                        if (!unicodes.empty()) pushCand(font->ttf.findGlyphIndex(unicodes[0]));
                        pushCand(cid);
                    } else if (font->ttf.hasCmap()) {
                        if (unicodes.size() == 1) {
                            pushCand(font->ttf.findGlyphIndex(unicodes[0]));
                        } else if (unicodes.size() == 2 && unicodes[0] == 'f') {
                            if (unicodes[1] == 'f') pushCand(font->ttf.findGlyphIndex(0xFB00));
                            else if (unicodes[1] == 'i') pushCand(font->ttf.findGlyphIndex(0xFB01));
                            else if (unicodes[1] == 'l') pushCand(font->ttf.findGlyphIndex(0xFB02));
                        }
                        pushCand(font->ttf.findGlyphByCharCode(cid));
                        pushCand(font->ttf.findGlyphIndex(cid));
                        if (nCands == 0 && font->toUnicode.empty() && font->encodingDiffs.empty()) {
                            if (font->firstChar > 0 && cid >= static_cast<uint16_t>(font->firstChar)) {
                                pushCand(static_cast<int>(cid) - font->firstChar + 1);
                            }
                            pushCand(cid);
                        }
                    } else {
                        pushCand(font->ttf.findGlyphByCharCode(cid));
                        if (font->firstChar > 0 && cid >= static_cast<uint16_t>(font->firstChar)) {
                            pushCand(static_cast<int>(cid) - font->firstChar + 1);
                        }
                        pushCand(cid);
                    }

                    if (nCands > 0) {
                        float scaleF = gs.fontSize / static_cast<float>(font->ttf.unitsPerEm());
                        float glyphLocal[6] = {
                            scaleF * gs.horizScale, 0.0f,
                            0.0f, scaleF,
                            0.0f, gs.textRise
                        };
                        float glyphToUser[6], glyphToGlobal[6], glyphToCanvas[6];
                        multiplyMatrix(glyphLocal, tm, glyphToUser);
                        multiplyMatrix(glyphToUser, gs.ctm, glyphToGlobal);
                        multiplyMatrix(glyphToGlobal, userToCanvas, glyphToCanvas);

                        for (int k = 0; k < nCands; ++k) {
                            if (font->ttf.renderGlyph(candGids[k], glyphToCanvas, inkGray, canvas)) {
                                ttfGid = candGids[k];
                                renderedByTtf = true;
                                stats.glyphsTtf++;
                                break;
                            }
                        }
                        if (ttfGid == 0) ttfGid = candGids[0];
                    }
                }

                // 3. Compute accurate glyph advance width in 1/1000 em PDF units before Unifont fallback
                int width1000 = 0;
                if (font) {
                    if (font->isTwoByte) {
                        auto cwIt = font->cidWidths.find(cid);
                        if (cwIt != font->cidWidths.end()) width1000 = cwIt->second;
                        else if (ttfGid > 0) width1000 = font->ttf.getGlyphWidth1000(ttfGid);
                        else width1000 = font->defaultWidth;
                    } else {
                        int idx = static_cast<int>(cid) - font->firstChar;
                        if (idx >= 0 && idx < static_cast<int>(font->widths.size())) {
                            width1000 = font->widths[idx];
                        } else if (ttfGid > 0) {
                            width1000 = font->ttf.getGlyphWidth1000(ttfGid);
                        } else if (font->defaultWidth > 0) {
                            width1000 = font->defaultWidth;
                        }
                    }
                }
                if (width1000 <= 0) {
                    uint32_t u0 = unicodes.empty() ? cid : unicodes[0];
                    width1000 = pdfStandardFontCharWidth(font ? font->baseFont : "", u0);
                }

                bool isSpace = (cid == 0x20 || (!unicodes.empty() && unicodes[0] == 0x20));
                float glyphAdv = ((static_cast<float>(width1000) / 1000.0f) * gs.fontSize +
                                  gs.charSpace + (isSpace ? gs.wordSpace : 0.0f)) * gs.horizScale;

                if (!isWhitespace && !renderedByTtf && !unicodes.empty()) {
                    // Expand ligatures (fi, fl, ff, ffi, ffl) when falling back to Unifont
                    if (unicodes.size() == 1) {
                        uint32_t u = unicodes[0];
                        if (u == 0xFB00) unicodes = { 'f', 'f' };
                        else if (u == 0xFB01) unicodes = { 'f', 'i' };
                        else if (u == 0xFB02) unicodes = { 'f', 'l' };
                        else if (u == 0xFB03) unicodes = { 'f', 'f', 'i' };
                        else if (u == 0xFB04) unicodes = { 'f', 'f', 'l' };
                    }
                    float totalAdvCanvasPx = std::fabs(glyphAdv) * xUnitToCanvasPx;
                    float subAdvCanvasPx = totalAdvCanvasPx / static_cast<float>(unicodes.size());
                    for (size_t uIdx = 0; uIdx < unicodes.size(); ++uIdx) {
                        uint32_t cp = unicodes[uIdx];
                        if (cp <= 0x20) continue;
                        float subOffsetEm = (unicodes.size() > 1)
                            ? (static_cast<float>(uIdx) * (glyphAdv / static_cast<float>(unicodes.size())))
                            : 0.0f;
                        float ux = tm[0] * subOffsetEm + tm[2] * gs.textRise + tm[4];
                        float uy = tm[1] * subOffsetEm + tm[3] * gs.textRise + tm[5];
                        PdfCanvas::Point pt = mapPointToCanvas(ux, uy);
                        drawUnifontCodepointSmooth(
                            canvas, pt.x, pt.y, effFontPx * 1.05f, subAdvCanvasPx, inkGray,
                            static_cast<int>(cp),
                            font ? font->isBold : false,
                            font ? font->isItalic : false);
                        stats.glyphsUnifont++;
                    }
                }

                tm[4] += glyphAdv * tm[0];
                tm[5] += glyphAdv * tm[1];
            }
        };

        std::vector<std::string> args;
        float pathCurU = 0.0f, pathCurV = 0.0f;
        bool pendingClip = false;

        const char *p = streamData.c_str();
        const char *end = p + streamData.size();

        while (p < end) {
            skipPdfWhitespace(p, end);
            if (p >= end) break;

            std::string tok;
            if (*p == '(') {
                const char *start = p;
                int pDepth = 0;
                while (p < end) {
                    if (*p == '\\' && p + 1 < end) p += 2;
                    else if (*p == '(') { pDepth++; p++; }
                    else if (*p == ')') { pDepth--; p++; if (pDepth == 0) break; }
                    else p++;
                }
                args.emplace_back(start, p);
                continue;
            } else if (*p == '<' && (p + 1 >= end || *(p + 1) != '<')) {
                const char *start = p++;
                while (p < end && *p != '>') p++;
                if (p < end) p++;
                args.emplace_back(start, p);
                continue;
            } else if (*p == '<' && p + 1 < end && *(p + 1) == '<') {
                size_t dStart = static_cast<size_t>(p - streamData.c_str());
                size_t dEnd = findMatchingDictEnd(streamData.c_str(), streamData.size(), dStart);
                if (dEnd == std::string::npos) break;
                p = streamData.c_str() + dEnd;
                continue;
            } else if (*p == '[') {
                const char *start = p;
                int aDepth = 0;
                while (p < end) {
                    if (*p == '(') {
                        int pDepth = 1;
                        p++;
                        while (p < end && pDepth > 0) {
                            if (*p == '\\' && p + 1 < end) p += 2;
                            else if (*p == '(') { pDepth++; p++; }
                            else if (*p == ')') { pDepth--; p++; }
                            else p++;
                        }
                    } else if (*p == '[') { aDepth++; p++; }
                    else if (*p == ']') { aDepth--; p++; if (aDepth == 0) break; }
                    else p++;
                }
                args.emplace_back(start, p);
                continue;
            } else if (*p == '/') {
                const char *start = p++;
                while (p < end && !isPdfWhitespace(*p) && !isPdfDelimiter(*p)) p++;
                args.emplace_back(start, p);
                continue;
            } else if (*p == '\'' || *p == '"') {
                tok.assign(p, p + 1);
                p++;
            } else {
                const char *start = p;
                bool startsNum = std::isdigit(static_cast<unsigned char>(*p)) ||
                                 ((*p == '-' || *p == '+' || *p == '.') && p + 1 < end &&
                                  (std::isdigit(static_cast<unsigned char>(*(p + 1))) || *(p + 1) == '.'));
                if (startsNum) {
                    bool seenDot = (*p == '.');
                    p++;
                    while (p < end && !isPdfWhitespace(*p) && !isPdfDelimiter(*p)) {
                        if (*p == '+' || *p == '-') {
                            if (p[-1] != 'e' && p[-1] != 'E') break; // split "0-12" into "0" and "-12"
                        } else if (*p == '.') {
                            if (seenDot) break; // split ".5.5" into ".5" and ".5"
                            seenDot = true;
                        } else if (std::isalpha(static_cast<unsigned char>(*p)) || *p == '\'' || *p == '"') {
                            if ((*p == 'e' || *p == 'E') && p + 1 < end &&
                                (std::isdigit(static_cast<unsigned char>(p[1])) || p[1] == '+' || p[1] == '-')) {
                                // scientific notation exponent
                            } else {
                                break; // split "0g" or "12Tf"
                            }
                        }
                        p++;
                    }
                } else {
                    while (p < end && !isPdfWhitespace(*p) && !isPdfDelimiter(*p) &&
                           *p != '\'' && *p != '"' && *p != '+' && *p != '-') {
                        p++;
                    }
                }
                if (p == start) { p++; continue; }
                tok.assign(start, p);
            }

            // Check if token is a number operand
            char firstCh = tok[0];
            if (std::isdigit(static_cast<unsigned char>(firstCh)) ||
                ((firstCh == '-' || firstCh == '+' || firstCh == '.') && tok.size() > 1 &&
                 (std::isdigit(static_cast<unsigned char>(tok[1])) || tok[1] == '.')))
            {
                args.push_back(std::move(tok));
                continue;
            }

            // Handle Inline Image: BI <key/val pairs> ID <bytes> EI
            if (tok == "BI") {
                args.clear();
                const char *idPos = p;
                while (idPos + 2 < end) {
                    if (isPdfWhitespace(idPos[0]) && idPos[1] == 'I' && idPos[2] == 'D' &&
                        (idPos + 3 >= end || isPdfWhitespace(idPos[3]) || isPdfDelimiter(idPos[3]))) {
                        break;
                    }
                    idPos++;
                }
                if (idPos + 3 >= end) break;
                std::string biDict = "<< " + std::string(p, idPos) + " >>";
                const char *dataStart = idPos + 3;
                if (dataStart < end && *dataStart == '\r' && dataStart + 1 < end && *(dataStart + 1) == '\n') {
                    dataStart += 2;
                } else if (dataStart < end && isPdfWhitespace(*dataStart)) {
                    dataStart++;
                }
                const char *eiPos = dataStart;
                while (eiPos + 2 <= end) {
                    if (isPdfWhitespace(eiPos[-1]) && eiPos[0] == 'E' && eiPos[1] == 'I' &&
                        (eiPos + 2 == end || isPdfWhitespace(eiPos[2]) || isPdfDelimiter(eiPos[2]))) {
                        break;
                    }
                    eiPos++;
                }
                if (eiPos > dataStart && eiPos + 2 <= end) {
                    size_t rawLen = static_cast<size_t>(eiPos - dataStart);
                    while (rawLen > 0 && (dataStart[rawLen - 1] == '\n' || dataStart[rawLen - 1] == '\r')) {
                        rawLen--;
                    }
                    int iw = atoi(extractDictValue(biDict, "W").c_str());
                    if (iw <= 0) iw = atoi(extractDictValue(biDict, "Width").c_str());
                    int ih = atoi(extractDictValue(biDict, "H").c_str());
                    if (ih <= 0) ih = atoi(extractDictValue(biDict, "Height").c_str());
                    int ibpc = atoi(extractDictValue(biDict, "BPC").c_str());
                    if (ibpc <= 0) ibpc = atoi(extractDictValue(biDict, "BitsPerComponent").c_str());
                    if (ibpc <= 0) ibpc = 8;
                    int icomps = resolveColorComponents(biDict);

                    std::vector<uint8_t> dec;
                    if (iw > 0 && ih > 0 &&
                        applyFilterChain(biDict, reinterpret_cast<const uint8_t *>(dataStart), rawLen, dec))
                    {
                        uint8_t *imgGray = nullptr;
                        bool isMask = false;
                        if (decodeRawPixelsToGrayBuffer(biDict, dec, iw, ih, ibpc, icomps, imgGray, isMask) && imgGray) {
                            float imgToCanvas[6];
                            multiplyMatrix(gs.ctm, userToCanvas, imgToCanvas);
                            canvas.drawImageAffine(imgGray, iw, ih, imgToCanvas, isMask, gs.fillGray);
                            free(imgGray);
                        }
                    }
                    p = eiPos + 2;
                } else {
                    break;
                }
                continue;
            }

            // Execute PDF operator `tok`
            const size_t nArgs = args.size();
            auto argF = [&](size_t idxFromEnd) -> float {
                return (idxFromEnd <= nArgs) ? strtof(args[nArgs - idxFromEnd].c_str(), nullptr) : 0.0f;
            };

            if (tok == "q") {
                canvas.getClipRect(gs.clipX0, gs.clipY0, gs.clipX1, gs.clipY1);
                if (gsStack.size() < 32) gsStack.push_back(gs);
            } else if (tok == "Q") {
                if (!gsStack.empty()) {
                    gs = gsStack.back();
                    gsStack.pop_back();
                    canvas.setClipRect(gs.clipX0, gs.clipY0, gs.clipX1, gs.clipY1);
                }
            } else if (tok == "cm" && nArgs >= 6) {
                float m[6] = { argF(6), argF(5), argF(4), argF(3), argF(2), argF(1) };
                multiplyMatrix(m, gs.ctm, gs.ctm);
            } else if (tok == "BT") {
                stats.btCount++;
                float id[6] = { 1, 0, 0, 1, 0, 0 };
                memcpy(tm, id, sizeof(id));
                memcpy(tlm, id, sizeof(id));
            } else if (tok == "Tm" && nArgs >= 6) {
                tm[0] = tlm[0] = argF(6);
                tm[1] = tlm[1] = argF(5);
                tm[2] = tlm[2] = argF(4);
                tm[3] = tlm[3] = argF(3);
                tm[4] = tlm[4] = argF(2);
                tm[5] = tlm[5] = argF(1);
            } else if ((tok == "Td" || tok == "TD") && nArgs >= 2) {
                float tx = argF(2), ty = argF(1);
                if (tok == "TD") gs.leading = -ty;
                tlm[4] += tx * tlm[0] + ty * tlm[2];
                tlm[5] += tx * tlm[1] + ty * tlm[3];
                memcpy(tm, tlm, sizeof(tlm));
            } else if (tok == "T*") {
                float ty = -gs.leading;
                tlm[4] += ty * tlm[2];
                tlm[5] += ty * tlm[3];
                memcpy(tm, tlm, sizeof(tlm));
            } else if (tok == "TL" && nArgs >= 1) {
                gs.leading = argF(1);
            } else if (tok == "Tc" && nArgs >= 1) {
                gs.charSpace = argF(1);
            } else if (tok == "Tw" && nArgs >= 1) {
                gs.wordSpace = argF(1);
            } else if (tok == "Tz" && nArgs >= 1) {
                gs.horizScale = argF(1) / 100.0f;
            } else if (tok == "Ts" && nArgs >= 1) {
                gs.textRise = argF(1);
            } else if (tok == "Tr" && nArgs >= 1) {
                gs.renderMode = static_cast<int>(argF(1));
            } else if (tok == "Tf" && nArgs >= 2) {
                stats.tfCount++;
                std::string fn = args[nArgs - 2];
                if (!fn.empty() && fn[0] == '/') fn.erase(0, 1);
                gs.fontName = fn;
                gs.fontSize = argF(1);
            } else if (tok == "Tj" && nArgs >= 1) {
                renderTextToken(args.back());
            } else if (tok == "'" && nArgs >= 1) {
                float ty = -gs.leading;
                tlm[4] += ty * tlm[2];
                tlm[5] += ty * tlm[3];
                memcpy(tm, tlm, sizeof(tlm));
                renderTextToken(args.back());
            } else if (tok == "\"" && nArgs >= 3) {
                gs.wordSpace = argF(3);
                gs.charSpace = argF(2);
                float ty = -gs.leading;
                tlm[4] += ty * tlm[2];
                tlm[5] += ty * tlm[3];
                memcpy(tm, tlm, sizeof(tlm));
                renderTextToken(args.back());
            } else if (tok == "TJ" && nArgs >= 1) {
                const std::string &arr = args.back();
                const char *ap = arr.c_str();
                const char *aend = ap + arr.size();
                if (ap < aend && *ap == '[') ap++;
                while (ap < aend) {
                    skipPdfWhitespace(ap, aend);
                    if (ap >= aend || *ap == ']') break;
                    if (*ap == '(') {
                        const char *sStart = ap;
                        int pDepth = 0;
                        while (ap < aend) {
                            if (*ap == '\\' && ap + 1 < aend) ap += 2;
                            else if (*ap == '(') { pDepth++; ap++; }
                            else if (*ap == ')') { pDepth--; ap++; if (pDepth == 0) break; }
                            else ap++;
                        }
                        renderTextToken(std::string(sStart, ap));
                    } else if (*ap == '<') {
                        const char *sStart = ap++;
                        while (ap < aend && *ap != '>') ap++;
                        if (ap < aend) ap++;
                        renderTextToken(std::string(sStart, ap));
                    } else if (std::isdigit(static_cast<unsigned char>(*ap)) || *ap == '-' || *ap == '+' || *ap == '.') {
                        char *endp = nullptr;
                        float adj = strtof(ap, &endp);
                        if (endp > ap) {
                            float delta = (-adj / 1000.0f) * gs.fontSize * gs.horizScale;
                            tm[4] += delta * tm[0];
                            tm[5] += delta * tm[1];
                            ap = endp;
                        } else {
                            ap++;
                        }
                    } else {
                        ap++;
                    }
                }
            } else if ((tok == "g" || tok == "G") && nArgs >= 1) {
                uint8_t v = static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, argF(1) * 255.0f)));
                if (tok == "g") gs.fillGray = v; else gs.strokeGray = v;
            } else if ((tok == "rg" || tok == "RG") && nArgs >= 3) {
                float r = argF(3), g = argF(2), b = argF(1);
                uint8_t v = static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, (0.299f * r + 0.587f * g + 0.114f * b) * 255.0f)));
                if (tok == "rg") gs.fillGray = v; else gs.strokeGray = v;
            } else if ((tok == "k" || tok == "K") && nArgs >= 4) {
                float c = argF(4), m = argF(3), y = argF(2), k = argF(1);
                float r = (1.0f - std::min(1.0f, c * (1.0f - k) + k));
                float g = (1.0f - std::min(1.0f, m * (1.0f - k) + k));
                float b = (1.0f - std::min(1.0f, y * (1.0f - k) + k));
                uint8_t v = static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, (0.299f * r + 0.587f * g + 0.114f * b) * 255.0f)));
                if (tok == "k") gs.fillGray = v; else gs.strokeGray = v;
            } else if (tok == "sc" || tok == "SC" || tok == "scn" || tok == "SCN") {
                bool isFill = (tok == "sc" || tok == "scn");
                size_t numCount = 0;
                for (size_t i = nArgs; i > 0; --i) {
                    const std::string &a = args[i - 1];
                    if (!a.empty() && (std::isdigit(static_cast<unsigned char>(a[0])) || a[0] == '.' || a[0] == '-')) numCount++;
                    else break;
                }
                if (numCount >= 4) {
                    float c = argF(4), m = argF(3), y = argF(2), k = argF(1);
                    float r = (1.0f - std::min(1.0f, c * (1.0f - k) + k));
                    float g = (1.0f - std::min(1.0f, m * (1.0f - k) + k));
                    float b = (1.0f - std::min(1.0f, y * (1.0f - k) + k));
                    uint8_t v = static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, (0.299f * r + 0.587f * g + 0.114f * b) * 255.0f)));
                    if (isFill) gs.fillGray = v; else gs.strokeGray = v;
                } else if (numCount == 3) {
                    float r = argF(3), g = argF(2), b = argF(1);
                    uint8_t v = static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, (0.299f * r + 0.587f * g + 0.114f * b) * 255.0f)));
                    if (isFill) gs.fillGray = v; else gs.strokeGray = v;
                } else if (numCount >= 1) {
                    uint8_t v = static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, argF(1) * 255.0f)));
                    if (isFill) gs.fillGray = v; else gs.strokeGray = v;
                }
            } else if (tok == "gs" && nArgs >= 1) {
                std::string gsName = args.back();
                if (!gsName.empty() && gsName[0] == '/') gsName.erase(0, 1);
                auto gsIt = extGsResMap.find(gsName);
                if (gsIt != extGsResMap.end()) {
                    std::string gsDict = gsIt->second.inlineDict;
                    if (gsIt->second.objId > 0 && gsDict.empty()) {
                        getObjectDict(gsIt->second.objId, gsDict);
                    }
                    if (!gsDict.empty()) {
                        std::string caStr = extractDictValue(gsDict, "ca");
                        if (!caStr.empty()) gs.fillAlpha = strtof(caStr.c_str(), nullptr);
                        std::string CAStr = extractDictValue(gsDict, "CA");
                        if (!CAStr.empty()) gs.strokeAlpha = strtof(CAStr.c_str(), nullptr);
                        std::string smaskVal = extractDictValue(gsDict, "SMask");
                        if (!smaskVal.empty()) {
                            gs.hasSoftMask = (smaskVal != "/None" && smaskVal != "None");
                        }
                    }
                }
            } else if (tok == "w" && nArgs >= 1) {
                gs.lineWidth = argF(1);
            } else if (tok == "d" && nArgs >= 2) {
                std::vector<float> rawDashes = parseFloatArray(args[nArgs - 2]);
                float ctmScale = std::hypot(gs.ctm[0], gs.ctm[1]) * canvasScale;
                gs.dashArray.clear();
                for (float dv : rawDashes) {
                    gs.dashArray.push_back(dv * ctmScale);
                }
                gs.dashPhase = argF(1) * ctmScale;
            } else if (tok == "m" && nArgs >= 2) {
                pathCurU = argF(2);
                pathCurV = argF(1);
                PdfCanvas::Point pt = mapPointToCanvas(pathCurU, pathCurV);
                canvas.moveTo(pt.x, pt.y);
            } else if (tok == "l" && nArgs >= 2) {
                pathCurU = argF(2);
                pathCurV = argF(1);
                PdfCanvas::Point pt = mapPointToCanvas(pathCurU, pathCurV);
                canvas.lineTo(pt.x, pt.y);
            } else if (tok == "c" && nArgs >= 6) {
                PdfCanvas::Point p1 = mapPointToCanvas(argF(6), argF(5));
                PdfCanvas::Point p2 = mapPointToCanvas(argF(4), argF(3));
                pathCurU = argF(2);
                pathCurV = argF(1);
                PdfCanvas::Point p3 = mapPointToCanvas(pathCurU, pathCurV);
                canvas.cubicTo(p1.x, p1.y, p2.x, p2.y, p3.x, p3.y);
            } else if (tok == "v" && nArgs >= 4) {
                PdfCanvas::Point p1 = mapPointToCanvas(pathCurU, pathCurV);
                PdfCanvas::Point p2 = mapPointToCanvas(argF(4), argF(3));
                pathCurU = argF(2);
                pathCurV = argF(1);
                PdfCanvas::Point p3 = mapPointToCanvas(pathCurU, pathCurV);
                canvas.cubicTo(p1.x, p1.y, p2.x, p2.y, p3.x, p3.y);
            } else if (tok == "y" && nArgs >= 4) {
                PdfCanvas::Point p1 = mapPointToCanvas(argF(4), argF(3));
                pathCurU = argF(2);
                pathCurV = argF(1);
                PdfCanvas::Point p3 = mapPointToCanvas(pathCurU, pathCurV);
                canvas.cubicTo(p1.x, p1.y, p3.x, p3.y, p3.x, p3.y);
            } else if (tok == "h") {
                canvas.closePath();
            } else if (tok == "re" && nArgs >= 4) {
                float rx = argF(4), ry = argF(3), rw = argF(2), rh = argF(1);
                PdfCanvas::Point p0 = mapPointToCanvas(rx, ry);
                PdfCanvas::Point p1 = mapPointToCanvas(rx + rw, ry);
                PdfCanvas::Point p2 = mapPointToCanvas(rx + rw, ry + rh);
                PdfCanvas::Point p3 = mapPointToCanvas(rx, ry + rh);
                canvas.moveTo(p0.x, p0.y);
                canvas.lineTo(p1.x, p1.y);
                canvas.lineTo(p2.x, p2.y);
                canvas.lineTo(p3.x, p3.y);
                canvas.closePath();
                pathCurU = rx;
                pathCurV = ry;
            } else if (tok == "W" || tok == "W*") {
                pendingClip = true;
            } else if (tok == "S" || tok == "s" || tok == "f" || tok == "F" || tok == "f*" ||
                       tok == "B" || tok == "B*" || tok == "b" || tok == "b*")
            {
                stats.pathCount++;
                if (tok == "s" || tok == "b" || tok == "b*") {
                    canvas.closePath();
                }
                bool doFill = (tok != "S" && tok != "s");
                bool doStroke = (tok == "S" || tok == "s" || tok == "B" || tok == "B*" || tok == "b" || tok == "b*");
                bool evenOdd = (tok == "f*" || tok == "B*" || tok == "b*");

                if (doFill && !gs.hasSoftMask && gs.fillAlpha > 0.05f) {
                    canvas.fillPath(gs.fillGray, evenOdd);
                }
                if (doStroke && !gs.hasSoftMask && gs.strokeAlpha > 0.05f) {
                    float effLw = gs.lineWidth * std::hypot(gs.ctm[0], gs.ctm[1]) * canvasScale;
                    canvas.strokePath(gs.strokeGray, effLw, gs.dashArray, gs.dashPhase);
                }
                if (pendingClip) {
                    canvas.intersectClipPathBBox();
                    pendingClip = false;
                }
                canvas.clearPath();
            } else if (tok == "n") {
                if (pendingClip) {
                    canvas.intersectClipPathBBox();
                    pendingClip = false;
                }
                canvas.clearPath();
            } else if (tok == "Do" && nArgs >= 1) {
                stats.doCount++;
                std::string xName = args.back();
                if (!xName.empty() && xName[0] == '/') xName.erase(0, 1);
                auto xIt = xobjResMap.find(xName);
                if (xIt != xobjResMap.end() && xIt->second.objId > 0) {
                    int xObjId = xIt->second.objId;
                    std::string xDict;
                    int64_t xStreamOff = -1;
                    if (getObjectDict(xObjId, xDict, &xStreamOff) && xStreamOff > 0) {
                        std::string subtype = extractDictValue(xDict, "Subtype");
                        if (subtype == "/Image" || subtype == "Image") {
                            uint8_t *imgGray = nullptr;
                            int iw = 0, ih = 0;
                            bool isMask = false;
                            if (decodeImageObjectToGray(xObjId, imgGray, iw, ih, isMask) && imgGray) {
                                float imgToCanvas[6];
                                multiplyMatrix(gs.ctm, userToCanvas, imgToCanvas);
                                canvas.drawImageAffine(imgGray, iw, ih, imgToCanvas, isMask, gs.fillGray);
                                free(imgGray);
                            }
                        } else if (subtype == "/Form" || subtype == "Form") {
                            std::vector<uint8_t> formDec;
                            if (readDecompressedStream(xDict, xStreamOff, formDec) && !formDec.empty()) {
                                GState savedGs = gs;
                                canvas.getClipRect(savedGs.clipX0, savedGs.clipY0, savedGs.clipX1, savedGs.clipY1);

                                std::vector<float> mat = parseFloatArray(extractDictValue(xDict, "Matrix"));
                                if (mat.size() >= 6) {
                                    float fm[6] = { mat[0], mat[1], mat[2], mat[3], mat[4], mat[5] };
                                    multiplyMatrix(fm, gs.ctm, gs.ctm);
                                }

                                std::string formRes = extractDictValue(xDict, "Resources");
                                int fResRef = parseRefObjId(formRes);
                                if (fResRef > 0) getObjectDict(fResRef, formRes);
                                if (formRes.empty()) formRes = resourcesDict;

                                std::string formStream(reinterpret_cast<const char *>(formDec.data()), formDec.size());
                                executeContentStream(formStream, formRes, userToCanvas, gs, canvas,
                                                     formDepth + 1, stats, &fontResMap, &xobjResMap, &extGsResMap);

                                gs = savedGs;
                                canvas.setClipRect(gs.clipX0, gs.clipY0, gs.clipX1, gs.clipY1);
                            }
                        }
                    }
                }
            }
            args.clear();
        }
    }

    bool rasterizeContentStreamPage(
        const PageEntry &entry, int maxW, int maxH,
        uint8_t *&outGrayBuf, int &outW, int &outH)
    {
        outGrayBuf = nullptr;
        std::string pageDict;
        if (!getObjectDict(entry.pageObjId, pageDict)) return false;

        // 1. Resolve MediaBox / CropBox (check page dict, cached inheritance, and /Parent chain)
        std::string boxStr = extractDictValue(pageDict, "CropBox");
        if (boxStr.empty()) boxStr = extractDictValue(pageDict, "MediaBox");
        if (boxStr.empty()) {
            auto boxIt = pageInheritedBox.find(entry.pageObjId);
            if (boxIt != pageInheritedBox.end()) boxStr = boxIt->second;
        }
        if (boxStr.empty()) {
            int pId = parseRefObjId(extractDictValue(pageDict, "Parent"));
            for (int up = 0; up < 5 && pId > 0 && boxStr.empty(); ++up) {
                std::string pDict;
                if (!getObjectDict(pId, pDict)) break;
                boxStr = extractDictValue(pDict, "CropBox");
                if (boxStr.empty()) boxStr = extractDictValue(pDict, "MediaBox");
                pId = parseRefObjId(extractDictValue(pDict, "Parent"));
            }
        }
        int boxRef = parseRefObjId(boxStr);
        if (boxRef > 0) boxStr = getObjectContent(boxRef);

        float llx = 0.0f, lly = 0.0f, urx = 612.0f, ury = 792.0f;
        std::vector<float> boxNums = parseFloatArray(boxStr);
        if (boxNums.size() >= 4) {
            llx = std::min(boxNums[0], boxNums[2]);
            urx = std::max(boxNums[0], boxNums[2]);
            lly = std::min(boxNums[1], boxNums[3]);
            ury = std::max(boxNums[1], boxNums[3]);
        }
        float pageW = std::max(72.0f, urx - llx);
        float pageH = std::max(72.0f, ury - lly);

        int rot = ((entry.rotate % 360) + 360) % 360;
        bool swapDims = (rot == 90 || rot == 270);
        float visW = swapDims ? pageH : pageW;
        float visH = swapDims ? pageW : pageH;

        computeFitDimensions(static_cast<int>(visW), static_cast<int>(visH), maxW, maxH, outW, outH);
        uint8_t *grayBuf = allocComicPageBuffer(static_cast<size_t>(outW) * outH);
        if (!grayBuf) return false;
        memset(grayBuf, 255, static_cast<size_t>(outW) * outH);

        // 2. Resolve /Resources (merging ancestor /Pages resources if page /Resources is partial)
        std::unordered_map<std::string, ResEntry> inheritedFontRes;
        std::unordered_map<std::string, ResEntry> inheritedXObjRes;
        std::unordered_map<std::string, ResEntry> inheritedExtGsRes;
        auto resIt = pageInheritedRes.find(entry.pageObjId);
        if (resIt != pageInheritedRes.end() && !resIt->second.empty()) {
            std::string inhRes = resIt->second;
            int iRef = parseRefObjId(inhRes);
            if (iRef > 0) getObjectDict(iRef, inhRes);
            parseResourceSubDict(inhRes, "Font", inheritedFontRes);
            parseResourceSubDict(inhRes, "XObject", inheritedXObjRes);
            parseResourceSubDict(inhRes, "ExtGState", inheritedExtGsRes);
        }
        int parentId = parseRefObjId(extractDictValue(pageDict, "Parent"));
        for (int up = 0; up < 5 && parentId > 0; ++up) {
            std::string pDict;
            if (!getObjectDict(parentId, pDict)) break;
            std::string pRes = extractDictValue(pDict, "Resources");
            int pRef = parseRefObjId(pRes);
            if (pRef > 0) getObjectDict(pRef, pRes);
            if (!pRes.empty()) {
                std::unordered_map<std::string, ResEntry> pFonts, pXObjs, pExtGs;
                parseResourceSubDict(pRes, "Font", pFonts);
                parseResourceSubDict(pRes, "XObject", pXObjs);
                parseResourceSubDict(pRes, "ExtGState", pExtGs);
                for (const auto &kv : pFonts) {
                    if (inheritedFontRes.find(kv.first) == inheritedFontRes.end()) {
                        inheritedFontRes[kv.first] = kv.second;
                    }
                }
                for (const auto &kv : pXObjs) {
                    if (inheritedXObjRes.find(kv.first) == inheritedXObjRes.end()) {
                        inheritedXObjRes[kv.first] = kv.second;
                    }
                }
                for (const auto &kv : pExtGs) {
                    if (inheritedExtGsRes.find(kv.first) == inheritedExtGsRes.end()) {
                        inheritedExtGsRes[kv.first] = kv.second;
                    }
                }
            }
            parentId = parseRefObjId(extractDictValue(pDict, "Parent"));
        }

        std::string pageRes = extractDictValue(pageDict, "Resources");
        if (pageRes.empty() && resIt != pageInheritedRes.end()) {
            pageRes = resIt->second;
        }
        int resRef = parseRefObjId(pageRes);
        if (resRef > 0) getObjectDict(resRef, pageRes);

        // 3. Decompress /Contents stream(s)
        std::string contentsVal = extractDictValue(pageDict, "Contents");
        std::vector<int> contentIds;
        int singleContentId = parseRefObjId(contentsVal);
        if (singleContentId > 0) {
            std::string cObj = getObjectContent(singleContentId);
            if (!cObj.empty() && cObj.find('[') != std::string::npos && cObj.find("<<") == std::string::npos) {
                contentIds = parseRefArray(cObj);
            } else {
                contentIds.push_back(singleContentId);
            }
        } else {
            contentIds = parseRefArray(contentsVal);
        }

        std::string fullStream;
        for (int cid : contentIds) {
            std::string cDict;
            int64_t cStreamOff = -1;
            if (getObjectDict(cid, cDict, &cStreamOff) && cStreamOff > 0) {
                std::vector<uint8_t> dec;
                if (readDecompressedStream(cDict, cStreamOff, dec) && !dec.empty()) {
                    fullStream.append(reinterpret_cast<const char *>(dec.data()), dec.size());
                    fullStream.push_back('\n');
                }
            }
        }

        if (fullStream.empty()) {
            logDiag("page[obj=" + std::to_string(entry.pageObjId) + "]: empty_content_stream (blank page)");
            outGrayBuf = grayBuf;
            return true;
        }

        // 4. Build user-space to canvas-pixel matrix (accounting for MediaBox origin & /Rotate)
        float userToCanvas[6];
        const float sx = static_cast<float>(outW) / visW;
        const float sy = static_cast<float>(outH) / visH;
        if (rot == 90) {
            userToCanvas[0] = 0.0f;
            userToCanvas[1] = sx;
            userToCanvas[2] = sy;
            userToCanvas[3] = 0.0f;
            userToCanvas[4] = -lly * sy;
            userToCanvas[5] = -llx * sx;
        } else if (rot == 180) {
            userToCanvas[0] = -sx;
            userToCanvas[1] = 0.0f;
            userToCanvas[2] = 0.0f;
            userToCanvas[3] = sy;
            userToCanvas[4] = urx * sx;
            userToCanvas[5] = -lly * sy;
        } else if (rot == 270) {
            userToCanvas[0] = 0.0f;
            userToCanvas[1] = -sx;
            userToCanvas[2] = -sy;
            userToCanvas[3] = 0.0f;
            userToCanvas[4] = ury * sy;
            userToCanvas[5] = urx * sx;
        } else {
            userToCanvas[0] = sx;
            userToCanvas[1] = 0.0f;
            userToCanvas[2] = 0.0f;
            userToCanvas[3] = -sy;
            userToCanvas[4] = -llx * sx;
            userToCanvas[5] = ury * sy;
        }

        PdfCanvas canvas(grayBuf, outW, outH);
        GState gs;
        canvas.getClipRect(gs.clipX0, gs.clipY0, gs.clipX1, gs.clipY1);

        StreamStats stats;
        executeContentStream(fullStream, pageRes, userToCanvas, gs, canvas, 0, stats,
                             &inheritedFontRes, &inheritedXObjRes, &inheritedExtGsRes);

        logDiag("page[obj=" + std::to_string(entry.pageObjId) + "]: canvas=" +
                std::to_string(outW) + "x" + std::to_string(outH) +
                " stm=" + std::to_string(fullStream.size()) +
                " BT=" + std::to_string(stats.btCount) +
                " Tf=" + std::to_string(stats.tfCount) +
                " Tj=" + std::to_string(stats.tjCount) +
                " g_ttf=" + std::to_string(stats.glyphsTtf) +
                " g_uni=" + std::to_string(stats.glyphsUnifont) +
                " Do=" + std::to_string(stats.doCount) +
                " paths=" + std::to_string(stats.pathCount) +
                " missF=" + std::to_string(stats.missingFonts));

        outGrayBuf = grayBuf;
        return true;
    }
};

PdfArchive::PdfArchive() : m_impl(new PdfImpl()) {
    m_impl->diagLog = &m_parseLog;
}

PdfArchive::PdfArchive(const std::string &fullPath) : m_impl(new PdfImpl()) {
    m_impl->diagLog = &m_parseLog;
    open(fullPath);
}

PdfArchive::~PdfArchive() {
    close();
    delete m_impl;
    m_impl = nullptr;
}

void PdfArchive::close() {
    if (m_impl) {
        m_impl->clearAll();
    }
    m_pages.clear();
    m_parseLog.clear();
    m_lastError.clear();
    m_title.clear();
    m_author.clear();
    m_path.clear();
}

bool PdfArchive::isOpen() const {
    return m_impl && (m_impl->isFileOpen() || !m_impl->filePath.empty()) && !m_pages.empty();
}

bool PdfArchive::open(const std::string &fullPath) {
    close();
    if (m_impl) m_impl->diagLog = &m_parseLog;
    m_path = fullPath;
    m_title = stripExtension(baseNameOf(fullPath));
    m_author = "PDF";

    m_impl->filePath = fullPath;

    // Obtain file size in O(1) via stat() BEFORE open() so FatFs reads the directory entry directly
    // without walking the FAT cluster chain via f_lseek(SEEK_END).
    m_impl->fileSize = 0;
    struct stat st{};
    if (stat(fullPath.c_str(), &st) == 0 && st.st_size > 0) {
        m_impl->fileSize = static_cast<int64_t>(st.st_size);
    }

    if (!m_impl->reopenFile()) {
        m_lastError = "Cannot open file: " + std::string(strerror(errno));
        m_parseLog.push_back("error=" + m_lastError);
        ESP_LOGE(TAG, "Failed to open PDF '%s': %s", fullPath.c_str(), m_lastError.c_str());
        return false;
    }

    if (m_impl->fileSize <= 0 && fstat(m_impl->fd, &st) == 0 && st.st_size > 0) {
        m_impl->fileSize = static_cast<int64_t>(st.st_size);
    }
    if (m_impl->fileSize <= 0) {
        off_t sz = ::lseek(m_impl->fd, 0, SEEK_END);
        if (sz > 0) {
            m_impl->fileSize = static_cast<int64_t>(sz);
            ::lseek(m_impl->fd, 0, SEEK_SET);
            m_impl->curPos = 0;
        } else {
            m_impl->reopenFile();
        }
    }

    if (m_impl->fileSize < 32) {
        m_lastError = "File is too small to be a PDF (" + std::to_string(m_impl->fileSize) + " bytes)";
        m_parseLog.push_back("error=" + m_lastError);
        m_impl->closeFile();
        return false;
    }

    std::string header = m_impl->readStringAt(0, 1024);
    if (header.find("%PDF-") == std::string::npos) {
        m_lastError = "Missing %PDF- header signature";
        m_parseLog.push_back("error=" + m_lastError);
        ESP_LOGE(TAG, "Invalid PDF header in '%s'", fullPath.c_str());
        m_impl->closeFile();
        return false;
    }

    m_parseLog.push_back("pdf_engine=v11");
    m_parseLog.push_back("file_size=" + std::to_string(m_impl->fileSize));

    bool ok = parseDocument();
    if (!ok || m_pages.empty()) {
        if (m_lastError.empty()) {
            m_lastError = "PDF opened, but found 0 renderable pages";
        }
        m_parseLog.push_back("error=" + m_lastError);
        ESP_LOGW(TAG, "PDF '%s' opened with 0 pages (%s)", m_title.c_str(), m_lastError.c_str());
        m_impl->closeFile(); // Close fd handle without wiping m_lastError / m_parseLog!
        return false;
    }

    const char *p0Fmt = "unknown";
    if (m_pages[0].format == ImageFormat::ContentStream) p0Fmt = "ContentStream";
    else if (m_pages[0].format == ImageFormat::Jpeg) p0Fmt = "Jpeg";
    else if (m_pages[0].format == ImageFormat::FlateJpeg) p0Fmt = "FlateJpeg";
    else if (m_pages[0].format == ImageFormat::Png) p0Fmt = "Png";
    else if (m_pages[0].format == ImageFormat::Jbig2) p0Fmt = "Jbig2";
    else if (m_pages[0].format == ImageFormat::FlateRaw) p0Fmt = "FlateRaw";
    m_parseLog.push_back(std::string("page0_mode=") + p0Fmt + " obj=" + std::to_string(m_pages[0].pageObjId));

    ESP_LOGI(TAG, "Opened PDF '%s' by '%s' (%zu pages, page0=%s)",
             m_title.c_str(), m_author.c_str(), m_pages.size(), p0Fmt);
    return true;
}

bool PdfArchive::ensurePageResolved(int pageIndex) {
    if (!m_impl || pageIndex < 0 || pageIndex >= static_cast<int>(m_pages.size())) {
        return false;
    }
    return m_impl->resolvePageEntry(m_pages[pageIndex]);
}

bool PdfArchive::parseDocument() {
    // -------------------------------------------------------------
    // Step 1: Locate startxref (check tail 1KB first, then 16KB, and linearized header)
    // -------------------------------------------------------------
    auto findStartxrefInTail = [](const std::string &s) -> int64_t {
        size_t sxPos = s.rfind("startxref");
        if (sxPos != std::string::npos) {
            const char *p = s.c_str() + sxPos + 9;
            const char *end = s.c_str() + s.size();
            skipPdfWhitespace(p, end);
            if (p < end && std::isdigit(static_cast<unsigned char>(*p))) {
                return strtoll(p, nullptr, 10);
            }
        }
        return -1;
    };

    int64_t tailSize = std::min(static_cast<int64_t>(1024), m_impl->fileSize);
    int64_t tailOffset = m_impl->fileSize - tailSize;
    std::string tail = m_impl->readStringAt(tailOffset, static_cast<size_t>(tailSize));
    int64_t startxrefPos = findStartxrefInTail(tail);

    if (startxrefPos <= 0 && m_impl->fileSize > 1024) {
        tailSize = std::min(static_cast<int64_t>(16384), m_impl->fileSize);
        tailOffset = m_impl->fileSize - tailSize;
        tail = m_impl->readStringAt(tailOffset, static_cast<size_t>(tailSize));
        startxrefPos = findStartxrefInTail(tail);
    }

    int rootObjId = -1;
    int infoObjId = -1;

    if (startxrefPos > 0 && startxrefPos < m_impl->fileSize) {
        m_parseLog.push_back("startxref=" + std::to_string(startxrefPos));
        m_impl->parseXRefChain(startxrefPos, rootObjId, infoObjId, m_parseLog);
    }

    // Also check if the file starts with a /Linearized dictionary (which has a first-page xref and /T main xref offset)
    std::string headChunk = m_impl->readStringAt(0, 2048);
    if (headChunk.find("/Linearized") != std::string::npos) {
        size_t xPos = headChunk.find("xref");
        if (xPos != std::string::npos) {
            m_impl->parseXRefChain(static_cast<int64_t>(xPos), rootObjId, infoObjId, m_parseLog);
        }
        size_t dStart = headChunk.find("<<");
        if (dStart != std::string::npos) {
            size_t dEnd = findMatchingDictEnd(headChunk.c_str(), headChunk.size(), dStart);
            if (dEnd != std::string::npos) {
                std::string linDict = headChunk.substr(dStart, dEnd - dStart);
                std::string tVal = extractDictValue(linDict, "T");
                if (!tVal.empty()) {
                    int64_t mainXref = strtoll(tVal.c_str(), nullptr, 10);
                    if (mainXref > 0 && mainXref < m_impl->fileSize) {
                        m_impl->parseXRefChain(mainXref, rootObjId, infoObjId, m_parseLog);
                    }
                }
            }
        }
    }

    // Fallback search for /Root in tail or header if trailer parsing didn't find it
    if (rootObjId <= 0) {
        size_t trPos = tail.rfind("trailer");
        std::string tailTrailer = (trPos != std::string::npos) ? tail.substr(trPos) : tail;
        rootObjId = parseRefObjId(extractDictValue(tailTrailer, "Root"));
        if (infoObjId <= 0) infoObjId = parseRefObjId(extractDictValue(tailTrailer, "Info"));
    }
    if (rootObjId <= 0) {
        size_t trPos = headChunk.find("trailer");
        std::string headTrailer = (trPos != std::string::npos) ? headChunk.substr(trPos) : headChunk;
        rootObjId = parseRefObjId(extractDictValue(headTrailer, "Root"));
        if (infoObjId <= 0) infoObjId = parseRefObjId(extractDictValue(headTrailer, "Info"));
    }

    // Parse /Info if present
    if (infoObjId > 0) {
        std::string infoDict;
        if (m_impl->getObjectDict(infoObjId, infoDict)) {
            std::string t = trimPdfString(decodePdfString(extractDictValue(infoDict, "Title")));
            std::string a = trimPdfString(decodePdfString(extractDictValue(infoDict, "Author")));
            if (!t.empty()) m_title = t;
            if (!a.empty()) m_author = a;
        }
    }

    // -------------------------------------------------------------
    // Step 2: Traverse Page Tree from /Root
    // -------------------------------------------------------------
    auto traversePageTree = [&](int catObjId, int directPagesObjId = -1) {
        int pagesId = directPagesObjId;
        if (pagesId <= 0 && catObjId > 0) {
            std::string rootDict;
            if (m_impl->getObjectDict(catObjId, rootDict)) {
                pagesId = parseRefObjId(extractDictValue(rootDict, "Pages"));
            }
        }
        if (pagesId <= 0) return;

        struct TreeItem {
            int objId;
            std::string inheritedRes;
            std::string inheritedBox;
            int inheritedRot;
        };
        std::vector<TreeItem> stack = { { pagesId, "", "", 0 } };
        std::unordered_set<int> visitedNodes;

        while (!stack.empty() && m_pages.size() < 4000) {
            TreeItem curr = std::move(stack.back());
            stack.pop_back();
            if (curr.objId <= 0 || visitedNodes.count(curr.objId)) continue;
            visitedNodes.insert(curr.objId);

            std::string nodeDict;
            if (!m_impl->getObjectDict(curr.objId, nodeDict)) continue;

            std::string resStr = extractDictValue(nodeDict, "Resources");
            if (resStr.empty()) resStr = curr.inheritedRes;
            std::string boxStr = extractDictValue(nodeDict, "CropBox");
            if (boxStr.empty()) boxStr = extractDictValue(nodeDict, "MediaBox");
            if (boxStr.empty()) boxStr = curr.inheritedBox;
            std::string rotStr = extractDictValue(nodeDict, "Rotate");
            int rotVal = !rotStr.empty() ? atoi(rotStr.c_str()) : curr.inheritedRot;

            std::string type = extractDictValue(nodeDict, "Type");
            std::string kidsStr = extractDictValue(nodeDict, "Kids");
            if (type == "/Pages" || type == "Pages" || !kidsStr.empty()) {
                int kidsRef = parseRefObjId(kidsStr);
                if (kidsRef > 0) kidsStr = m_impl->getObjectContent(kidsRef);
                std::vector<int> kids = parseRefArray(kidsStr);
                int countVal = atoi(extractDictValue(nodeDict, "Count").c_str());

                // Optimization for large PDFs: if /Count == kids.size() and kids are uncompressed
                // leaf /Page objects on disk, verify the first kid and register the siblings lazily
                // so we don't seek across a 100MB+ SD card file for every page during indexing!
                bool allLeafPagesOnDisk = (countVal > 1 && countVal == static_cast<int>(kids.size()));
                if (allLeafPagesOnDisk) {
                    for (int kidId : kids) {
                        auto xIt = m_impl->xref.find(kidId);
                        if (xIt == m_impl->xref.end() || xIt->second.type != 1) {
                            allLeafPagesOnDisk = false;
                            break;
                        }
                    }
                }

                if (allLeafPagesOnDisk) {
                    for (int kidId : kids) {
                        if (!resStr.empty()) m_impl->pageInheritedRes[kidId] = resStr;
                        if (!boxStr.empty()) m_impl->pageInheritedBox[kidId] = boxStr;
                        if (rotVal != 0) m_impl->pageInheritedRotate[kidId] = rotVal;
                        PageEntry pe;
                        pe.pageObjId = kidId;
                        pe.resolved = false;
                        if (m_pages.empty()) {
                            // Always resolve the very first page immediately to verify validity
                            if (m_impl->resolvePageEntry(pe)) {
                                m_pages.push_back(pe);
                            }
                        } else {
                            m_pages.push_back(pe);
                        }
                    }
                } else {
                    for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
                        stack.push_back({ *it, resStr, boxStr, rotVal });
                    }
                }
            } else if (type == "/Page" || type == "Page" || !extractDictValue(nodeDict, "Contents").empty()) {
                if (!resStr.empty()) m_impl->pageInheritedRes[curr.objId] = resStr;
                if (!boxStr.empty()) m_impl->pageInheritedBox[curr.objId] = boxStr;
                if (rotVal != 0) m_impl->pageInheritedRotate[curr.objId] = rotVal;
                PageEntry pe;
                pe.pageObjId = curr.objId;
                pe.resolved = false;
                if (m_pages.empty()) {
                    if (m_impl->resolvePageEntry(pe)) {
                        m_pages.push_back(pe);
                    }
                } else {
                    m_pages.push_back(pe);
                }
            }
        }

        // Ensure at least one page in m_pages is verified resolved
        if (!m_pages.empty() && !m_pages[0].resolved) {
            if (!m_impl->resolvePageEntry(m_pages[0])) {
                m_pages.clear();
            }
        }
    };

    if (rootObjId > 0) {
        traversePageTree(rootObjId);
    }

    if (!m_pages.empty()) {
        m_parseLog.push_back("tree_parsed_pages=" + std::to_string(m_pages.size()));
        return true;
    }

    // -------------------------------------------------------------
    // Step 3: Fallback Linear Scanner
    // If xref table or page tree resolution produced 0 pages, scan
    // the PDF file sequentially for indirect objects ("<id> <gen> obj")
    // to rebuild xref and collect /Type /Page objects (or /Subtype /Image).
    // -------------------------------------------------------------
    ESP_LOGI(TAG, "Page tree produced 0 pages, running linear object & page scan...");
    m_parseLog.push_back("fallback_scan_started");

    constexpr size_t CHUNK_SIZE = 32768;
    std::vector<char> buffer(CHUNK_SIZE + 512);
    int64_t filePos = 0;
    std::vector<int> scannedPageObjIds;
    std::unordered_set<int> seenPageIds;
    std::vector<int> deferredObjStmIds;
    std::vector<int64_t> candidateImageOffsets;
    int topPagesObjId = -1;
    size_t processedObjStmOrderIdx = 0;

    auto harvestUnpackedObjStmObjects = [&]() {
        while (processedObjStmOrderIdx < m_impl->objStmOrder.size()) {
            int uOid = m_impl->objStmOrder[processedObjStmOrderIdx++];
            auto cIt = m_impl->objStmCache.find(uOid);
            if (cIt == m_impl->objStmCache.end()) continue;
            const std::string &s = cIt->second;
            std::string tVal = extractDictValue(s, "Type");
            if ((tVal == "/Catalog" || tVal == "Catalog") && rootObjId <= 0) {
                rootObjId = uOid;
            } else if ((tVal == "/Pages" || tVal == "Pages") && extractDictValue(s, "Parent").empty() && topPagesObjId <= 0) {
                topPagesObjId = uOid;
            } else if ((tVal == "/Page" || tVal == "Page") && extractDictValue(s, "Kids").empty()) {
                if (seenPageIds.insert(uOid).second) {
                    scannedPageObjIds.push_back(uOid);
                }
            }
        }
    };

#ifdef ESP_PLATFORM
    int chunkCounter = 0;
#endif

    while (filePos < m_impl->fileSize && scannedPageObjIds.size() < 4000) {
#ifdef ESP_PLATFORM
        if ((++chunkCounter & 15) == 0) {
            vTaskDelay(1);
        }
#endif
        size_t bytesRead = m_impl->readAt(filePos, buffer.data(), CHUNK_SIZE);
        if (bytesRead < 16) break;

        const char *bufStart = buffer.data();
        const char *end = bufStart + bytesRead;
        const char *p = bufStart;

        // First: index any "<id> <gen> obj" headers in this chunk if missing from xref
        while (p < end - 8) {
            const char *oPos = static_cast<const char *>(memchr(p, 'o', static_cast<size_t>((end - 4) - p)));
            if (!oPos) break;
            if (oPos > bufStart && isPdfWhitespace(oPos[-1]) &&
                oPos[1] == 'b' && oPos[2] == 'j' &&
                (isPdfWhitespace(oPos[3]) || isPdfDelimiter(oPos[3])))
            {
                // Walk back over generation and object number
                const char *b = oPos - 1;
                while (b > bufStart && isPdfWhitespace(*b)) b--;
                while (b > bufStart && std::isdigit(static_cast<unsigned char>(*b))) b--;
                while (b > bufStart && isPdfWhitespace(*b)) b--;
                while (b > bufStart && std::isdigit(static_cast<unsigned char>(b[-1]))) b--;
                int oid = 0, gen = 0;
                if (sscanf(b, "%d %d obj", &oid, &gen) == 2 && oid > 0) {
                    int64_t objOff = filePos + static_cast<int64_t>(b - bufStart);
                    if (m_impl->xref.find(oid) == m_impl->xref.end()) {
                        m_impl->xref[oid] = { 1, objOff, gen };
                    }
                    // Check if this object is a /Type /Page, /Type /ObjStm, /Type /Catalog, or /Type /XRef dictionary
                    const char *dictLim = std::min(end, oPos + 1024);
                    std::string snippet(oPos, dictLim);
                    std::string tVal = extractDictValue(snippet, "Type");
                    if ((tVal == "/Page" || tVal == "Page") && extractDictValue(snippet, "Kids").empty()) {
                        if (seenPageIds.insert(oid).second) {
                            scannedPageObjIds.push_back(oid);
                        }
                    } else if (tVal == "/ObjStm" || tVal == "ObjStm") {
                        // Unpack ObjStm immediately in RAM from buffer if it fits; otherwise defer until after sequential scan
                        if (m_impl->tryLoadObjStmFromBuffer(oid, b, end)) {
                            harvestUnpackedObjStmObjects();
                        } else if (std::find(deferredObjStmIds.begin(), deferredObjStmIds.end(), oid) == deferredObjStmIds.end()) {
                            deferredObjStmIds.push_back(oid);
                        }
                    } else if ((tVal == "/Catalog" || tVal == "Catalog") && rootObjId <= 0) {
                        rootObjId = oid;
                    } else if ((tVal == "/Pages" || tVal == "Pages") && extractDictValue(snippet, "Parent").empty() && topPagesObjId <= 0) {
                        topPagesObjId = oid;
                    } else if ((tVal == "/XRef" || tVal == "XRef") && rootObjId <= 0) {
                        int rId = parseRefObjId(extractDictValue(snippet, "Root"));
                        if (rId > 0) rootObjId = rId;
                    }
                }
            }
            p = oPos + 1;
        }

        // Record candidate standalone /Image offsets without seeking away from filePos during the sequential scan
        p = bufStart;
        while (p < end - 12 && candidateImageOffsets.size() < 512) {
            const char *imgTag = static_cast<const char *>(memchr(p, '/', static_cast<size_t>((end - 6) - p)));
            if (!imgTag) break;
            if (memcmp(imgTag, "/Image", 6) != 0 ||
                (imgTag + 6 < end && !isPdfWhitespace(imgTag[6]) && !isPdfDelimiter(imgTag[6])))
            {
                p = imgTag + 1;
                continue;
            }

            const char *back = imgTag;
            const char *minBack = std::max(bufStart, imgTag - 1024);
            const char *objPos = nullptr;
            while (back >= minBack + 3) {
                if (back[0] == 'o' && back[1] == 'b' && back[2] == 'j' &&
                    isPdfWhitespace(back[-1]) &&
                    (isPdfWhitespace(back[3]) || isPdfDelimiter(back[3])))
                {
                    objPos = back;
                    break;
                }
                back--;
            }

            if (objPos) {
                int64_t objOffset = filePos + static_cast<int64_t>(objPos - bufStart);
                if (candidateImageOffsets.empty() || candidateImageOffsets.back() != objOffset) {
                    candidateImageOffsets.push_back(objOffset);
                }
            }
            p = imgTag + 6;
        }

        filePos += (bytesRead > 1024) ? static_cast<int64_t>(bytesRead - 512) : static_cast<int64_t>(bytesRead);
    }

    // Unpack any /ObjStm streams that straddled chunk boundaries during the sequential scan
    for (int stmId : deferredObjStmIds) {
        if (m_impl->loadObjStm(stmId)) {
            harvestUnpackedObjStmObjects();
        }
    }
    harvestUnpackedObjStmObjects();

    // Prefer page tree traversal if /Catalog or root /Pages was found during linear scan or ObjStm unpacking
    if (rootObjId > 0 || topPagesObjId > 0) {
        traversePageTree(rootObjId, topPagesObjId);
    }

    if (m_pages.empty()) {
        for (int pid : scannedPageObjIds) {
            PageEntry pe;
            pe.pageObjId = pid;
            pe.resolved = false;
            if (m_pages.empty()) {
                if (m_impl->resolvePageEntry(pe)) {
                    m_pages.push_back(pe);
                }
            } else {
                m_pages.push_back(pe);
            }
        }
    }

    if (m_pages.empty() && !candidateImageOffsets.empty()) {
        for (int64_t imgOff : candidateImageOffsets) {
            PageEntry entry;
            int idx = static_cast<int>(m_pages.size() + 1);
            if (m_impl->parseImageObjAtOffset(idx, idx, imgOff, entry)) {
                if (entry.streamLen >= 2048 || (entry.width >= 150 && entry.height >= 150)) {
                    m_pages.push_back(entry);
                }
            }
        }
    }

    m_parseLog.push_back("fallback_pages=" + std::to_string(m_pages.size()));
    return !m_pages.empty();
}

namespace {

// Converts a Flate-compressed raw bitmap stream into an 8-bit grayscale buffer
static bool decodeFlateRawToPsramGray(
    const PdfArchive::PageEntry &entry,
    const uint8_t *rawBytes,
    size_t rawLen,
    uint8_t *&outGrayBuf,
    int &outW,
    int &outH)
{
    outGrayBuf = nullptr;
    if (entry.width <= 0 || entry.height <= 0 || !rawBytes || rawLen == 0) {
        return false;
    }

    int w = entry.width;
    int h = entry.height;
    int bpc = (entry.bitsPerComponent > 0) ? entry.bitsPerComponent : 8;
    int comps = (entry.colorComponents > 0) ? entry.colorComponents : 3;
    int predictor = entry.predictor;
    int predCols = (entry.predictorColumns > 0) ? entry.predictorColumns : w;
    std::vector<uint8_t> emptyPal;
    if (inflatePdfFlateImageToGray(rawBytes, rawLen, w, h, bpc, comps, predictor, predCols, false, emptyPal, outGrayBuf, outW, outH)) {
        return true;
    }

    size_t uncompLen = 0;
    uint8_t *uncomp = inflatePdfStream(rawBytes, rawLen, uncompLen);
    if (!uncomp || uncompLen == 0) {
        if (uncomp) free(uncomp);
        return false;
    }

    // Self-correct colorComponents and PNG predictor from actual decompressed byte length
    size_t pixels = static_cast<size_t>(w) * h;
    if (bpc == 8 && h > 0) {
        for (int testComps : { comps, 1, 3, 4 }) {
            size_t exactRaw = pixels * static_cast<size_t>(testComps);
            size_t exactPng = (static_cast<size_t>(w) * testComps + 1) * static_cast<size_t>(h);
            if (uncompLen == exactPng) {
                comps = testComps;
                if (predictor < 10) predictor = 12;
                break;
            } else if (uncompLen == exactRaw) {
                comps = testComps;
                if (predictor >= 10) predictor = 1;
                break;
            }
        }
    }

    std::vector<uint8_t> filtered;
    const uint8_t *pixelBytes = uncomp;
    size_t pixelBytesLen = uncompLen;

    if (predictor > 1) {
        int cols = (entry.predictorColumns > 0) ? entry.predictorColumns : w;
        if (unfilterPdfPredictor(uncomp, uncompLen, predictor, cols, comps, bpc, filtered) && !filtered.empty()) {
            pixelBytes = filtered.data();
            pixelBytesLen = filtered.size();
        }
    }

    uint8_t *grayBuf = allocComicPageBuffer(pixels);
    if (!grayBuf) {
        free(uncomp);
        return false;
    }

    if (bpc <= 1) {
        int stride = (w + 7) / 8;
        for (int y = 0; y < h; ++y) {
            const uint8_t *row = pixelBytes + static_cast<size_t>(y) * stride;
            uint8_t *dstRow = grayBuf + static_cast<size_t>(y) * w;
            if (static_cast<size_t>(y + 1) * stride > pixelBytesLen) {
                memset(dstRow, 255, static_cast<size_t>(w));
                continue;
            }
            for (int x = 0; x < w; ++x) {
                bool bit = ((row[x / 8] >> (7 - (x % 8))) & 1) != 0;
                dstRow[x] = bit ? 255 : 0;
            }
        }
    } else if (comps == 1 && bpc == 8) {
        size_t toCopy = std::min(pixels, pixelBytesLen);
        memcpy(grayBuf, pixelBytes, toCopy);
        if (toCopy < pixels) memset(grayBuf + toCopy, 255, pixels - toCopy);
    } else if (comps == 3 && bpc == 8) {
        size_t maxPx = std::min(pixels, pixelBytesLen / 3);
        for (size_t i = 0; i < maxPx; ++i) {
            uint8_t r = pixelBytes[i * 3];
            uint8_t g = pixelBytes[i * 3 + 1];
            uint8_t b = pixelBytes[i * 3 + 2];
            grayBuf[i] = static_cast<uint8_t>((r * 77 + g * 150 + b * 29) >> 8);
        }
        if (maxPx < pixels) memset(grayBuf + maxPx, 255, pixels - maxPx);
    } else if (comps == 4 && bpc == 8) {
        size_t maxPx = std::min(pixels, pixelBytesLen / 4);
        for (size_t i = 0; i < maxPx; ++i) {
            uint8_t c = pixelBytes[i * 4];
            uint8_t m = pixelBytes[i * 4 + 1];
            uint8_t y = pixelBytes[i * 4 + 2];
            uint8_t k = pixelBytes[i * 4 + 3];
            uint8_t r = 255 - std::min(255, (c * (255 - k)) / 255 + k);
            uint8_t g = 255 - std::min(255, (m * (255 - k)) / 255 + k);
            uint8_t b = 255 - std::min(255, (y * (255 - k)) / 255 + k);
            grayBuf[i] = static_cast<uint8_t>((r * 77 + g * 150 + b * 29) >> 8);
        }
        if (maxPx < pixels) memset(grayBuf + maxPx, 255, pixels - maxPx);
    } else {
        memset(grayBuf, 255, pixels);
    }

    free(uncomp);
    outGrayBuf = grayBuf;
    outW = w;
    outH = h;
    return true;
}

} // namespace

bool PdfArchive::renderPage(int pageIndex, Renderer *renderer, unsigned char *targetFrameBuffer) {
    if (!isOpen() || !renderer || !targetFrameBuffer) {
        return false;
    }
    if (!ensurePageResolved(pageIndex)) {
        m_parseLog.push_back("render_fail[p=" + std::to_string(pageIndex) + "]: resolvePageEntry failed");
        return false;
    }

    const PageEntry &entry = m_pages[pageIndex];

    if (entry.format == ImageFormat::ContentStream) {
        uint8_t *grayBuf = nullptr;
        int outW = 0, outH = 0;
        if (!m_impl->rasterizeContentStreamPage(entry, EPD_HEIGHT, EPD_WIDTH, grayBuf, outW, outH) || !grayBuf) {
            m_parseLog.push_back("render_fail[p=" + std::to_string(pageIndex) + " obj=" + std::to_string(entry.pageObjId) + "]: rasterize failed");
            return false;
        }

        // Apply in-place mid-tone Floyd-Steinberg dithering for gray fills and embedded images while keeping crisp text
        std::vector<int16_t> errCur(outW + 2, 0);
        std::vector<int16_t> errNext(outW + 2, 0);
        for (int y = 0; y < outH; ++y) {
            std::fill(errNext.begin(), errNext.end(), 0);
            uint8_t *row = grayBuf + static_cast<size_t>(y) * outW;
            for (int x = 0; x < outW; ++x) {
                uint8_t orig = row[x];
                if (orig >= 244) {
                    row[x] = 255;
                    continue;
                }
                if (orig <= 16) {
                    row[x] = 0;
                    continue;
                }
                int val = std::max(0, std::min(255, static_cast<int>(orig) + (errCur[x + 1] >> 4)));
                uint8_t outPx = (val < 140) ? 0 : 255;
                row[x] = outPx;
                int err = val - static_cast<int>(outPx);
                errCur[x + 2]  = static_cast<int16_t>(errCur[x + 2] + err * 7);
                errNext[x]     = static_cast<int16_t>(errNext[x] + err * 3);
                errNext[x + 1] = static_cast<int16_t>(errNext[x + 1] + err * 5);
                errNext[x + 2] = static_cast<int16_t>(errNext[x + 2] + err * 1);
            }
            errCur.swap(errNext);
        }

        int startX = (EPD_HEIGHT - outW) / 2;
        int startY = (EPD_WIDTH - outH) / 2;
        renderer->framebuffer = targetFrameBuffer;
        for (int y = 0; y < outH; ++y) {
            int screenY = (EPD_WIDTH - 1) - y - startY;
            const uint8_t *row = grayBuf + static_cast<size_t>(y) * outW;
            for (int x = 0; x < outW; ++x) {
                renderer->drawPixel(x + startX, screenY, row[x] > 128);
            }
        }
        free(grayBuf);
        return true;
    }

    if (entry.streamLen == 0 || entry.streamOffset <= 0) {
        return false;
    }

    uint8_t *rawBytes = allocComicPageBuffer(entry.streamLen + 4);
    if (!rawBytes) {
        ESP_LOGE(TAG, "Failed to allocate %zu bytes for PDF page %d", entry.streamLen, pageIndex);
        return false;
    }

    size_t n = m_impl->readAt(entry.streamOffset, rawBytes, entry.streamLen);
    if (n < entry.streamLen) {
        ESP_LOGW(TAG, "Short read on PDF page %d: expected %zu, got %zu", pageIndex, entry.streamLen, n);
    }

    if (entry.format == ImageFormat::FlateJpeg) {
        size_t uncompLen = 0;
        uint8_t *jpgHeap = inflatePdfStream(rawBytes, n, uncompLen);
        free(rawBytes);
        if (!jpgHeap || uncompLen == 0) {
            if (jpgHeap) free(jpgHeap);
            return false;
        }
        rawBytes = allocComicPageBuffer(uncompLen + 4);
        if (!rawBytes) {
            free(jpgHeap);
            return false;
        }
        memcpy(rawBytes, jpgHeap, uncompLen);
        n = uncompLen;
        free(jpgHeap);
    }

    if (entry.format == ImageFormat::Jpeg || entry.format == ImageFormat::FlateJpeg) {
        size_t offset = 0;
        while (offset + 1 < n && (rawBytes[offset] != 0xFF || rawBytes[offset + 1] != 0xD8)) {
            offset++;
        }
        if (offset > 0 && offset + 2 < n) {
            memmove(rawBytes, rawBytes + offset, n - offset);
            n -= offset;
        }
        return decodeAndRenderComicPageBuffer(rawBytes, n, "page.jpg", renderer, targetFrameBuffer);
    } else if (entry.format == ImageFormat::Png) {
        return decodeAndRenderComicPageBuffer(rawBytes, n, "page.png", renderer, targetFrameBuffer);
    } else if (entry.format == ImageFormat::FlateRaw || entry.format == ImageFormat::Jbig2) {
        uint8_t *grayBuf = nullptr;
        int srcW = 0, srcH = 0;
        bool ok = (entry.format == ImageFormat::Jbig2)
            ? decodePdfJbig2ToGray(rawBytes, n, entry.width, entry.height, false, grayBuf, srcW, srcH)
            : decodeFlateRawToPsramGray(entry, rawBytes, n, grayBuf, srcW, srcH);
        free(rawBytes);
        if (!ok || !grayBuf) return false;

        int outW = 0, outH = 0;
        computeFitDimensions(srcW, srcH, EPD_HEIGHT, EPD_WIDTH, outW, outH);
        std::vector<uint8_t> scaledGray(static_cast<size_t>(outW) * outH);
        resizeGrayscaleBilinearToPtr(grayBuf, srcW, srcH, scaledGray.data(), outW, outH);
        free(grayBuf);

        enhanceAndDitherComicGray(scaledGray, outW, outH);
        blitDitheredToFrameBuffer(renderer, targetFrameBuffer, scaledGray, outW, outH);
        return true;
    }

    free(rawBytes);
    return false;
}

bool PdfArchive::loadPageZoomCache(int pageIndex, ComicZoomPageCache &outCache) {
    if (!isOpen()) {
        return false;
    }
    if (pageIndex < 0 || pageIndex >= static_cast<int>(m_pages.size())) {
        outCache.clear();
        return false;
    }
    if (outCache.isValidFor(pageIndex)) {
        return true;
    }

    outCache.clear();
    if (!ensurePageResolved(pageIndex)) {
        return false;
    }

    const PageEntry &entry = m_pages[pageIndex];

    if (entry.format == ImageFormat::ContentStream) {
        uint8_t *grayBuf = nullptr;
        int imgW = 0, imgH = 0;
        if (!m_impl->rasterizeContentStreamPage(entry, EPD_HEIGHT * 2, EPD_WIDTH * 2, grayBuf, imgW, imgH) || !grayBuf) {
            return false;
        }
        outCache.grayBuf = grayBuf;
        outCache.width = imgW;
        outCache.height = imgH;
        computeFitDimensions(imgW, imgH, EPD_HEIGHT, EPD_WIDTH, outCache.fitW, outCache.fitH);
        outCache.pageIndex = pageIndex;
        return true;
    }

    if (entry.streamLen == 0 || entry.streamOffset <= 0) {
        return false;
    }

    uint8_t *rawBytes = allocComicPageBuffer(entry.streamLen + 4);
    if (!rawBytes) {
        return false;
    }

    size_t n = m_impl->readAt(entry.streamOffset, rawBytes, entry.streamLen);

    if (entry.format == ImageFormat::FlateJpeg) {
        size_t uncompLen = 0;
        uint8_t *jpgHeap = inflatePdfStream(rawBytes, n, uncompLen);
        free(rawBytes);
        if (!jpgHeap || uncompLen == 0) {
            if (jpgHeap) free(jpgHeap);
            return false;
        }
        rawBytes = allocComicPageBuffer(uncompLen + 4);
        if (!rawBytes) {
            free(jpgHeap);
            return false;
        }
        memcpy(rawBytes, jpgHeap, uncompLen);
        n = uncompLen;
        free(jpgHeap);
    }

    if (entry.format == ImageFormat::Jpeg || entry.format == ImageFormat::FlateJpeg) {
        size_t offset = 0;
        while (offset + 1 < n && (rawBytes[offset] != 0xFF || rawBytes[offset + 1] != 0xD8)) {
            offset++;
        }
        if (offset > 0 && offset + 2 < n) {
            memmove(rawBytes, rawBytes + offset, n - offset);
            n -= offset;
        }
        return decodeComicPageToZoomCache(rawBytes, n, "page.jpg", pageIndex, outCache);
    } else if (entry.format == ImageFormat::Png) {
        return decodeComicPageToZoomCache(rawBytes, n, "page.png", pageIndex, outCache);
    } else if (entry.format == ImageFormat::FlateRaw || entry.format == ImageFormat::Jbig2) {
        uint8_t *grayBuf = nullptr;
        int srcW = 0, srcH = 0;
        bool ok = (entry.format == ImageFormat::Jbig2)
            ? decodePdfJbig2ToGray(rawBytes, n, entry.width, entry.height, false, grayBuf, srcW, srcH)
            : decodeFlateRawToPsramGray(entry, rawBytes, n, grayBuf, srcW, srcH);
        free(rawBytes);
        if (!ok || !grayBuf) return false;

        constexpr int MAX_CACHE_W = EPD_HEIGHT * 2;
        constexpr int MAX_CACHE_H = EPD_WIDTH * 2;
        if (srcW > MAX_CACHE_W || srcH > MAX_CACHE_H) {
            int dstW = 0, dstH = 0;
            computeFitDimensions(srcW, srcH, MAX_CACHE_W, MAX_CACHE_H, dstW, dstH);
            uint8_t *resized = allocComicPageBuffer(static_cast<size_t>(dstW) * dstH);
            if (resized) {
                resizeGrayscaleBilinearToPtr(grayBuf, srcW, srcH, resized, dstW, dstH);
                free(grayBuf);
                grayBuf = resized;
                srcW = dstW;
                srcH = dstH;
            }
        }

        outCache.grayBuf = grayBuf;
        outCache.width = srcW;
        outCache.height = srcH;
        computeFitDimensions(srcW, srcH, EPD_HEIGHT, EPD_WIDTH, outCache.fitW, outCache.fitH);
        outCache.pageIndex = pageIndex;
        return true;
    }

    free(rawBytes);
    return false;
}
