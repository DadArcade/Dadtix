#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// -----------------------------------------------------------------------------
// PDF Stream Filter Decoders (Flate/Zlib in Flash, ASCIIHex, ASCII85, RunLength, LZW)
// -----------------------------------------------------------------------------
// Decompresses a PDF /FlateDecode stream using Flash-resident zlib (never ESP32-S3 ROM miniz).
// Returns a buffer allocated via allocComicPageBuffer() (free with standard free()).
uint8_t *inflatePdfDeflateToPsram(const uint8_t *src, size_t srcLen, size_t &outLen);

// Streams a /FlateDecode raw image (1/2/4/8-bit Gray, RGB, CMYK, or Indexed with optional
// PNG/TIFF predictor) directly into an 8-bit grayscale buffer (downscaling if > 1296px)
// using only a 32KB sliding window so multi-megabyte RGB images never exhaust PSRAM.
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
    int &outH);

// Decodes a PDF /JBIG2Decode bi-level image stream (ITU-T T.88 Generic Region with MQ
// arithmetic coding) into an 8-bit grayscale buffer (downscaled if > 1296px).
bool decodePdfJbig2ToGray(
    const uint8_t *src,
    size_t srcLen,
    int fallbackW,
    int fallbackH,
    bool invertBits,
    uint8_t *&outGrayBuf,
    int &outW,
    int &outH);

bool decodePdfAsciiHex(const uint8_t *src, size_t len, std::vector<uint8_t> &out);
bool decodePdfAscii85(const uint8_t *src, size_t len, std::vector<uint8_t> &out);
bool decodePdfRunLength(const uint8_t *src, size_t len, std::vector<uint8_t> &out);
bool decodePdfLzw(const uint8_t *src, size_t len, int earlyChange, std::vector<uint8_t> &out);

// -----------------------------------------------------------------------------
// PDF Character Encodings & Standard Adobe Glyph Name Resolution
// -----------------------------------------------------------------------------
uint32_t pdfWinAnsiToUnicode(uint8_t code);
uint32_t pdfMacRomanToUnicode(uint8_t code);
uint32_t pdfGlyphNameToUnicode(const std::string &glyphName);
int pdfStandardFontCharWidth(const std::string &baseFont, uint32_t unicodeCp);

// -----------------------------------------------------------------------------
// 2D Anti-Aliased Grayscale Vector Canvas (Scanline Polygon, Bezier & Image)
// -----------------------------------------------------------------------------
class PdfCanvas {
public:
    struct Point {
        float x;
        float y;
    };

    struct Subpath {
        std::vector<Point> pts;
        bool closed = false;
    };

    PdfCanvas(uint8_t *grayBuf, int width, int height);

    int width() const { return m_width; }
    int height() const { return m_height; }
    uint8_t *buffer() const { return m_buf; }

    void setClipRect(int x0, int y0, int x1, int y1);
    void resetClipRect();
    void getClipRect(int &x0, int &y0, int &x1, int &y1) const;
    void intersectClipPathBBox();

    // Path construction in canvas pixel coordinates
    void clearPath();
    void moveTo(float x, float y);
    void lineTo(float x, float y);
    void quadTo(float x1, float y1, float x2, float y2);
    void cubicTo(float x1, float y1, float x2, float y2, float x3, float y3);
    void rect(float x, float y, float w, float h);
    void closePath();
    bool hasPath() const { return !m_subpaths.empty(); }

    // 4x supersampled scanline polygon fill (Non-Zero Winding or Even-Odd)
    void fillPath(uint8_t fillGray, bool evenOdd = false, float coverageBoost = 1.0f);

    // Anti-aliased polyline/polygon stroking with thickness and optional dash pattern
    void strokePath(
        uint8_t strokeGray,
        float lineWidth,
        const std::vector<float> &dashArray = {},
        float dashPhase = 0.0f);

    // Composites an 8-bit grayscale image (or 1-bit stencil mask) transformed by
    // the 2D affine matrix `imgToCanvas[6]` mapping unit square [0,1]x[0,1] to canvas pixels.
    void drawImageAffine(
        const uint8_t *srcGray,
        int srcW,
        int srcH,
        const float imgToCanvas[6],
        bool isStencilMask = false,
        uint8_t maskInkGray = 0);

    // Blends a single pixel with coverage alpha (0..255) within the active clip rect
    inline void blendPixel(int x, int y, uint8_t gray, uint8_t alpha) {
        if (x < m_clipX0 || x >= m_clipX1 || y < m_clipY0 || y >= m_clipY1 || alpha == 0) {
            return;
        }
        uint8_t &dst = m_buf[y * m_width + x];
        if (alpha == 255) {
            dst = gray;
        } else {
            dst = static_cast<uint8_t>((static_cast<int>(gray) * alpha + static_cast<int>(dst) * (255 - alpha) + 127) / 255);
        }
    }

private:
    uint8_t *m_buf = nullptr;
    int m_width = 0;
    int m_height = 0;
    int m_clipX0 = 0;
    int m_clipY0 = 0;
    int m_clipX1 = 0;
    int m_clipY1 = 0;

    std::vector<Subpath> m_subpaths;
    float m_curX = 0.0f;
    float m_curY = 0.0f;
    float m_startX = 0.0f;
    float m_startY = 0.0f;
    bool m_hasCurrentPoint = false;

    void flattenQuadRecursive(float x0, float y0, float x1, float y1, float x2, float y2, int depth);
    void flattenCubicRecursive(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, int depth);
    void strokeSegmentAA(float x0, float y0, float x1, float y1, uint8_t gray, float width);
};

// -----------------------------------------------------------------------------
// Embedded TrueType (/FontFile2) & CFF/Type1C (/FontFile3) Outline Rasterizer
// -----------------------------------------------------------------------------
class PdfTrueTypeFont {
public:
    PdfTrueTypeFont() = default;

    bool load(std::vector<uint8_t> &&fontData);
    bool isValid() const { return m_valid; }
    bool isCff() const { return m_isCff; }
    bool hasCmap() const { return (m_cmapOffset > 0 && m_cmapLen >= 4) || !m_cffUnicodeToGid.empty(); }
    int unitsPerEm() const { return m_unitsPerEm; }
    int numGlyphs() const { return m_numGlyphs; }

    // Returns true if glyph `gid` has a non-empty contour outline in the 'glyf' or 'CFF ' table
    bool hasGlyphOutline(int gid) const;

    // Looks up glyph index via embedded 'cmap' table or CFF charset/Unicode map (returns 0 if unmapped)
    int findGlyphIndex(uint32_t unicodeCp) const;

    // Looks up glyph index by PostScript glyph name (e.g. from /Encoding /Differences in CFF fonts)
    int findGlyphByName(const std::string &glyphName) const;

    // Looks up glyph index by 1-byte/2-byte character code or CID in CFF encoding/charset tables
    int findGlyphByCharCode(uint16_t code) const;

    // Reads glyph horizontal advance width (in 1/1000 em PDF units)
    int getGlyphWidth1000(int gid) const;

    // Renders glyph `gid` onto `canvas` using the 2D affine matrix `fontUnitToCanvas[6]`
    // mapping font design units (x, y) to canvas pixel coordinates.
    bool renderGlyph(
        int gid,
        const float fontUnitToCanvas[6],
        uint8_t inkGray,
        PdfCanvas &canvas) const;

    struct CffIndex {
        uint32_t dataStart = 0;
        std::vector<uint32_t> offsets; // 0-based offsets relative to dataStart (size = count + 1)
        size_t count() const { return offsets.empty() ? 0 : (offsets.size() - 1); }
    };

private:
    std::vector<uint8_t> m_data;
    bool m_valid = false;
    bool m_isCff = false;
    bool m_cffIsCid = false;
    int m_unitsPerEm = 1000;
    int m_indexToLocFormat = 0;
    int m_numGlyphs = 0;
    int m_numHMetrics = 0;

    uint32_t m_cmapOffset = 0;
    uint32_t m_cmapLen = 0;
    uint32_t m_locaOffset = 0;
    uint32_t m_locaLen = 0;
    uint32_t m_glyfOffset = 0;
    uint32_t m_glyfLen = 0;
    uint32_t m_hmtxOffset = 0;
    uint32_t m_hmtxLen = 0;

    // CFF (/Type1C and OpenType 'CFF ') state
    uint32_t m_cffOffset = 0;
    uint32_t m_cffLen = 0;
    CffIndex m_cffCharStrings;
    CffIndex m_cffGlobalSubrs;
    CffIndex m_cffLocalSubrs;
    std::vector<CffIndex> m_cffFdSubrs;
    std::vector<uint8_t> m_cffGlyphFd;
    std::unordered_map<uint32_t, int> m_cffUnicodeToGid;
    std::unordered_map<std::string, int> m_cffNameToGid;
    std::unordered_map<uint16_t, int> m_cffCodeToGid;

    bool parseCff(uint32_t cffOffset, uint32_t cffLen);
    bool getGlyphLocation(int gid, uint32_t &outOffset, uint32_t &outLen) const;
    bool appendGlyphContours(int gid, const float matrix[6], PdfCanvas &canvas, int depth) const;
    bool appendCffGlyphContours(int gid, const float matrix[6], PdfCanvas &canvas) const;
};
