#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "cbzHandler.h"
#include "renderer.h"
#include "miniz_custom.h"
#include "tinyxml2.h"
#include "tjpgd.h"
#include "PNGdec.h"
#undef Trace

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <algorithm>
#include <cctype>
#include <cstring>

static const char *TAG = "CBZ";

struct CbzArchive::ZipImpl {
    mz_zip_archive archive;
    bool openFlag = false;

    ZipImpl() {
        memset(&archive, 0, sizeof(archive));
    }

    ~ZipImpl() {
        if (openFlag) {
            mz_zip_reader_end(&archive);
            openFlag = false;
        }
    }
};

namespace {

static std::string toLowerAscii(const std::string &s) {
    std::string out = s;
    for (char &c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

static std::string baseNameOf(const std::string &path) {
    size_t pos = path.find_last_of("/\\");
    return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

static std::string stripExtension(const std::string &filename) {
    size_t dot = filename.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return filename;
    return filename.substr(0, dot);
}

static void computeFitDimensions(int srcW, int srcH, int maxW, int maxH, int &outW, int &outH) {
    if (srcW <= 0 || srcH <= 0) {
        outW = maxW;
        outH = maxH;
        return;
    }

    if (static_cast<int64_t>(srcW) * maxH >= static_cast<int64_t>(srcH) * maxW) {
        outW = maxW;
        outH = static_cast<int>((static_cast<int64_t>(srcH) * maxW + srcW / 2) / srcW);
    } else {
        outH = maxH;
        outW = static_cast<int>((static_cast<int64_t>(srcW) * maxH + srcH / 2) / srcH);
    }

    if (outW < 1) outW = 1;
    if (outH < 1) outH = 1;
    if (outW > maxW) outW = maxW;
    if (outH > maxH) outW = maxH;
}

static void resizeGrayscaleBilinearToPtr(
    const uint8_t *src, int srcW, int srcH,
    uint8_t *dstOut, int dstW, int dstH)
{
    const int FP_SHIFT = 16;
    const int FP_ONE = 1 << FP_SHIFT;

    int xRatio = (srcW > 1) ? (((srcW - 1) << FP_SHIFT) / dstW) : 0;
    int yRatio = (srcH > 1) ? (((srcH - 1) << FP_SHIFT) / dstH) : 0;

    for (int y = 0; y < dstH; ++y) {
        int srcY = (y * yRatio) >> FP_SHIFT;
        int yFrac = (y * yRatio) & (FP_ONE - 1);
        int dstRow = y * dstW;
        int srcRow = srcY * srcW;
        int srcRowNext = (srcY + 1 < srcH) ? (srcY + 1) * srcW : srcRow;

        for (int x = 0; x < dstW; ++x) {
            int srcX = (x * xRatio) >> FP_SHIFT;
            int xFrac = (x * xRatio) & (FP_ONE - 1);

            int a = src[srcRow + srcX];
            int b = (srcX + 1 < srcW) ? src[srcRow + srcX + 1] : a;
            int c = src[srcRowNext + srcX];
            int d = (srcX + 1 < srcW) ? src[srcRowNext + srcX + 1] : c;

            int top = a + (((b - a) * xFrac) >> FP_SHIFT);
            int bottom = c + (((d - c) * xFrac) >> FP_SHIFT);
            dstOut[dstRow + x] = static_cast<uint8_t>(top + (((bottom - top) * yFrac) >> FP_SHIFT));
        }
    }
}

static void resizeGrayscaleBilinear(
    const uint8_t *src, int srcW, int srcH,
    std::vector<uint8_t> &dstOut, int dstW, int dstH)
{
    dstOut.resize(static_cast<size_t>(dstW) * dstH);
    resizeGrayscaleBilinearToPtr(src, srcW, srcH, dstOut.data(), dstW, dstH);
}

struct CbzJpegCtx {
    const uint8_t *data;
    size_t size;
    size_t offset;
    uint8_t *outBuf;
    int outW;
    int outH;
};

static size_t cbzJpegInFunc(JDEC *jd, uint8_t *buf, size_t len) {
    CbzJpegCtx *ctx = static_cast<CbzJpegCtx *>(jd->device);
    if (ctx->offset + len > ctx->size) {
        len = ctx->size - ctx->offset;
    }
    if (buf && len > 0) {
        memcpy(buf, ctx->data + ctx->offset, len);
    }
    ctx->offset += len;
    return len;
}

static int cbzJpegOutFunc(JDEC *jd, void *bitmap, JRECT *rect) {
    CbzJpegCtx *ctx = static_cast<CbzJpegCtx *>(jd->device);
    const uint8_t *src = static_cast<const uint8_t *>(bitmap);
    int bw = rect->right - rect->left + 1;
    int bh = rect->bottom - rect->top + 1;

    for (int y = 0; y < bh; ++y) {
        int py = rect->top + y;
        if (py >= ctx->outH) continue;
        uint8_t *dstRow = ctx->outBuf + py * ctx->outW;
        const uint8_t *srcRow = src + 3 * y * bw;
        for (int x = 0; x < bw; ++x) {
            int px = rect->left + x;
            if (px >= ctx->outW) continue;
            uint8_t r = srcRow[3 * x + 0];
            uint8_t g = srcRow[3 * x + 1];
            uint8_t b = srcRow[3 * x + 2];
            dstRow[px] = static_cast<uint8_t>((r * 77 + g * 150 + b * 29) >> 8);
        }
    }
    return 1;
}

static bool decodeJpegEntryToPsram(
    uint8_t *jpegBuf, size_t jpegSize,
    int maxW, int maxH,
    uint8_t *&outGrayBuf, int &outW, int &outH)
{
    outGrayBuf = nullptr;
    constexpr size_t WORK_BUF_SIZE = 16384;
    uint8_t *workBuf = static_cast<uint8_t *>(heap_caps_malloc(WORK_BUF_SIZE, MALLOC_CAP_8BIT));
    if (!workBuf) {
        free(jpegBuf);
        return false;
    }

    JDEC jd;
    CbzJpegCtx ctx{jpegBuf, jpegSize, 0, nullptr, 0, 0};

    JRESULT res = jd_prepare(&jd, cbzJpegInFunc, workBuf, WORK_BUF_SIZE, &ctx);
    if (res != JDR_OK || jd.width == 0 || jd.height == 0) {
        ESP_LOGE(TAG, "JPEG prepare failed: %d", res);
        free(workBuf);
        free(jpegBuf);
        return false;
    }

    computeFitDimensions(jd.width, jd.height, maxW, maxH, outW, outH);

    uint8_t scale = 0;
    while (scale < 3 &&
           (jd.width >> (scale + 1)) >= outW &&
           (jd.height >> (scale + 1)) >= outH) {
        ++scale;
    }

    int decW = jd.width >> scale;
    int decH = jd.height >> scale;
    size_t decPixels = static_cast<size_t>(decW) * decH;

    uint8_t *decBuf = allocComicPageBuffer(decPixels);
    if (!decBuf && scale < 3) {
        // Fallback to smaller hardware IDCT scale if PSRAM is tight
        ++scale;
        decW = jd.width >> scale;
        decH = jd.height >> scale;
        decPixels = static_cast<size_t>(decW) * decH;
        if (outW > decW || outH > decH) {
            computeFitDimensions(jd.width, jd.height, decW, decH, outW, outH);
        }
        decBuf = allocComicPageBuffer(decPixels);
    }
    if (!decBuf) {
        ESP_LOGE(TAG, "Failed to allocate %zu bytes for decoded JPEG (%dx%d)", decPixels, decW, decH);
        free(workBuf);
        free(jpegBuf);
        return false;
    }

    ctx.outBuf = decBuf;
    ctx.outW = decW;
    ctx.outH = decH;

    res = jd_decomp(&jd, cbzJpegOutFunc, scale);
    // Immediately free compressed JPEG and work buffer BEFORE resizing/dithering
    free(jpegBuf);
    free(workBuf);

    if (res != JDR_OK) {
        ESP_LOGE(TAG, "JPEG decode failed: %d", res);
        free(decBuf);
        return false;
    }

    if (decW == outW && decH == outH) {
        outGrayBuf = decBuf;
        return true;
    }

    size_t outPixels = static_cast<size_t>(outW) * outH;
    uint8_t *resized = allocComicPageBuffer(outPixels);
    if (!resized) {
        // Keep decBuf at decW x decH if resize buffer allocation fails
        outGrayBuf = decBuf;
        outW = decW;
        outH = decH;
        return true;
    }

    resizeGrayscaleBilinearToPtr(decBuf, decW, decH, resized, outW, outH);
    free(decBuf);
    outGrayBuf = resized;
    return true;
}

static bool decodeJpegEntryAndFreeInput(
    uint8_t *jpegBuf, size_t jpegSize,
    int maxW, int maxH,
    std::vector<uint8_t> &outGray, int &outW, int &outH)
{
    uint8_t *psramBuf = nullptr;
    if (!decodeJpegEntryToPsram(jpegBuf, jpegSize, maxW, maxH, psramBuf, outW, outH) || !psramBuf) {
        return false;
    }
    size_t total = static_cast<size_t>(outW) * outH;
    outGray.assign(psramBuf, psramBuf + total);
    free(psramBuf);
    return true;
}

struct CbzPngCtx {
    uint8_t *rowBuf;
    int outW;
    int outH;
    int srcW;
};

static CbzPngCtx *s_cbzPngCtx = nullptr;

static void cbzPngLineCallback(PNGDRAW *pDraw) {
    CbzPngCtx *ctx = s_cbzPngCtx;
    if (!ctx || !ctx->rowBuf || pDraw->y >= ctx->outH) return;

    const uint8_t *src = static_cast<const uint8_t *>(pDraw->pPixels);
    int bpp = pDraw->iPitch / pDraw->iWidth;
    uint8_t *dstRow = ctx->rowBuf + pDraw->y * ctx->outW;

    for (int x = 0; x < ctx->outW; ++x) {
        int srcX = (x * ctx->srcW) / ctx->outW;
        uint8_t gray = 255;
        if (bpp == 1) {
            gray = src[srcX];
        } else if (bpp == 2) {
            uint8_t g = src[srcX * 2 + 0];
            uint8_t a = src[srcX * 2 + 1];
            gray = static_cast<uint8_t>((g * a + 255 * (255 - a)) >> 8);
        } else if (bpp == 3) {
            uint8_t r = src[srcX * 3 + 0];
            uint8_t g = src[srcX * 3 + 1];
            uint8_t b = src[srcX * 3 + 2];
            gray = static_cast<uint8_t>((r * 77 + g * 150 + b * 29) >> 8);
        } else if (bpp == 4) {
            uint8_t r = src[srcX * 4 + 0];
            uint8_t g = src[srcX * 4 + 1];
            uint8_t b = src[srcX * 4 + 2];
            uint8_t a = src[srcX * 4 + 3];
            uint8_t lum = static_cast<uint8_t>((r * 77 + g * 150 + b * 29) >> 8);
            gray = static_cast<uint8_t>((lum * a + 255 * (255 - a)) >> 8);
        }
        dstRow[x] = gray;
    }
}

static bool decodePngEntryToPsram(
    uint8_t *pngBuf, size_t pngSize,
    int maxW, int maxH,
    uint8_t *&outGrayBuf, int &outW, int &outH)
{
    outGrayBuf = nullptr;
    if (pngSize < 24) {
        free(pngBuf);
        return false;
    }
    int srcW = (pngBuf[16] << 24) | (pngBuf[17] << 16) | (pngBuf[18] << 8) | pngBuf[19];
    int srcH = (pngBuf[20] << 24) | (pngBuf[21] << 16) | (pngBuf[22] << 8) | pngBuf[23];
    if (srcW <= 0 || srcH <= 0 || srcW > 10000 || srcH > 10000) {
        free(pngBuf);
        return false;
    }

    computeFitDimensions(srcW, srcH, maxW, maxH, outW, outH);

    size_t rowPixels = static_cast<size_t>(outW) * srcH;
    uint8_t *rowBuf = allocComicPageBuffer(rowPixels);
    if (!rowBuf) {
        free(pngBuf);
        return false;
    }

    PNG png;
    CbzPngCtx ctx{rowBuf, outW, srcH, srcW};
    s_cbzPngCtx = &ctx;

    int rc = png.openRAM(pngBuf, static_cast<int>(pngSize), cbzPngLineCallback);
    if (rc != PNG_SUCCESS) {
        s_cbzPngCtx = nullptr;
        free(pngBuf);
        free(rowBuf);
        return false;
    }

    png.decode(nullptr, 0);
    png.close();
    s_cbzPngCtx = nullptr;
    free(pngBuf);

    if (srcH == outH) {
        outGrayBuf = rowBuf;
        return true;
    }

    size_t outPixels = static_cast<size_t>(outW) * outH;
    uint8_t *resized = allocComicPageBuffer(outPixels);
    if (!resized) {
        free(rowBuf);
        return false;
    }
    resizeGrayscaleBilinearToPtr(rowBuf, outW, srcH, resized, outW, outH);
    free(rowBuf);
    outGrayBuf = resized;
    return true;
}

static bool decodePngEntryAndFreeInput(
    uint8_t *pngBuf, size_t pngSize,
    int maxW, int maxH,
    std::vector<uint8_t> &outGray, int &outW, int &outH)
{
    uint8_t *psramBuf = nullptr;
    if (!decodePngEntryToPsram(pngBuf, pngSize, maxW, maxH, psramBuf, outW, outH) || !psramBuf) {
        return false;
    }
    size_t total = static_cast<size_t>(outW) * outH;
    outGray.assign(psramBuf, psramBuf + total);
    free(psramBuf);
    return true;
}

static void normalizeComicGrayContrast(uint8_t *gray, size_t total) {
    if (!gray || total == 0) return;

    uint32_t hist[256] = {0};
    for (size_t i = 0; i < total; ++i) {
        hist[gray[i]]++;
    }

    uint32_t lowTarget = static_cast<uint32_t>((total * 3) / 100);   // 3.0% dark ink clip
    uint32_t highTarget = static_cast<uint32_t>((total * 85) / 100); // 85.0% paper white clip

    int blackPt = 0;
    uint32_t acc = 0;
    for (int i = 0; i < 256; ++i) {
        acc += hist[i];
        if (acc >= lowTarget) {
            blackPt = i;
            break;
        }
    }

    int whitePt = 255;
    acc = 0;
    for (int i = 0; i < 256; ++i) {
        acc += hist[i];
        if (acc >= highTarget) {
            whitePt = i;
            break;
        }
    }

    if (whitePt > blackPt + 32) {
        uint8_t lut[256];
        int span = whitePt - blackPt;
        for (int i = 0; i < 256; ++i) {
            if (i <= blackPt) lut[i] = 0;
            else if (i >= whitePt) lut[i] = 255;
            else lut[i] = static_cast<uint8_t>(((i - blackPt) * 255 + span / 2) / span);
        }
        for (size_t i = 0; i < total; ++i) {
            gray[i] = lut[gray[i]];
        }
    }
}

static void ditherComicGray(std::vector<uint8_t> &gray, int width, int height) {
    const size_t total = gray.size();
    if (total == 0 || width <= 0 || height <= 0) return;

    std::vector<int16_t> currLine(width, 0);
    std::vector<int16_t> nextLine(width, 0);

    for (int y = 0; y < height; ++y) {
        uint8_t *row = gray.data() + y * width;
        for (int x = 0; x < width; ++x) {
            int oldPixel = static_cast<int>(row[x]) + currLine[x];
            if (oldPixel < 0) oldPixel = 0;
            if (oldPixel > 255) oldPixel = 255;

            int newPixel = (oldPixel < 128) ? 0 : 255;
            row[x] = static_cast<uint8_t>(newPixel);

            int err = oldPixel - newPixel;
            if (x + 1 < width) {
                currLine[x + 1] += static_cast<int16_t>((err * 7 + 8) >> 4);
            }
            if (y + 1 < height) {
                if (x > 0) {
                    nextLine[x - 1] += static_cast<int16_t>((err * 3 + 8) >> 4);
                }
                nextLine[x] += static_cast<int16_t>((err * 5 + 8) >> 4);
                if (x + 1 < width) {
                    nextLine[x + 1] += static_cast<int16_t>((err * 1 + 8) >> 4);
                }
            }
        }
        currLine.swap(nextLine);
        std::fill(nextLine.begin(), nextLine.end(), 0);
    }
}

static void enhanceAndDitherComicGray(std::vector<uint8_t> &gray, int width, int height) {
    if (gray.empty() || width <= 0 || height <= 0) return;
    normalizeComicGrayContrast(gray.data(), gray.size());
    ditherComicGray(gray, width, height);
}

static void blitDitheredToFrameBuffer(
    Renderer *renderer,
    unsigned char *frameBuffer,
    const std::vector<uint8_t> &dithered,
    int imgW,
    int imgH)
{
    int startX = (EPD_HEIGHT - imgW) / 2;
    int startY = (EPD_WIDTH - imgH) / 2;
    renderer->framebuffer = frameBuffer;

    for (int y = 0; y < imgH; ++y) {
        int screenY = (EPD_WIDTH - 1) - y - startY;
        const uint8_t *row = dithered.data() + y * imgW;
        for (int x = 0; x < imgW; ++x) {
            renderer->drawPixel(x + startX, screenY, row[x] > 128);
        }
    }
}

} // namespace

uint8_t *allocComicPageBuffer(size_t size) {
    uint8_t *p = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!p) {
        p = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_8BIT));
    }
    return p;
}

bool isComicImageEntry(const std::string &entryName) {
    if (entryName.empty() || entryName.back() == '/' || entryName.back() == '\\') {
        return false;
    }

    if (entryName.find("__MACOSX/") != std::string::npos ||
        entryName.find("__MACOSX\\") != std::string::npos) {
        return false;
    }

    std::string base = baseNameOf(entryName);
    if (base.empty() || base[0] == '.') {
        return false;
    }

    size_t dot = base.find_last_of('.');
    if (dot == std::string::npos) {
        return false;
    }

    std::string ext = toLowerAscii(base.substr(dot + 1));
    return (ext == "jpg" || ext == "jpeg" || ext == "png");
}

bool naturalLessComicEntry(const std::string &a, const std::string &b) {
    size_t ia = 0, ib = 0;
    const size_t na = a.size(), nb = b.size();

    while (ia < na && ib < nb) {
        unsigned char ca = static_cast<unsigned char>(a[ia]);
        unsigned char cb = static_cast<unsigned char>(b[ib]);

        if (std::isdigit(ca) && std::isdigit(cb)) {
            while (ia < na && a[ia] == '0') ++ia;
            while (ib < nb && b[ib] == '0') ++ib;

            size_t za = ia, zb = ib;
            while (ia < na && std::isdigit(static_cast<unsigned char>(a[ia]))) ++ia;
            while (ib < nb && std::isdigit(static_cast<unsigned char>(b[ib]))) ++ib;

            size_t lenA = ia - za;
            size_t lenB = ib - zb;
            if (lenA != lenB) return lenA < lenB;

            int cmp = strncmp(a.c_str() + za, b.c_str() + zb, lenA);
            if (cmp != 0) return cmp < 0;
        } else {
            int la = std::tolower(ca);
            int lb = std::tolower(cb);
            if (la != lb) return la < lb;
            ++ia;
            ++ib;
        }
    }
    return (na - ia) < (nb - ib);
}

std::string defaultComicTitleFromPath(const std::string &archivePath) {
    return stripExtension(baseNameOf(archivePath));
}

void parseComicInfoXml(const char *xmlData, size_t xmlLen, std::string &ioTitle, std::string &ioAuthor) {
    tinyxml2::XMLDocument doc;
    if (doc.Parse(xmlData, xmlLen) != tinyxml2::XML_SUCCESS) {
        return;
    }

    tinyxml2::XMLElement *root = doc.FirstChildElement("ComicInfo");
    if (!root) return;

    const char *series = nullptr;
    const char *number = nullptr;
    const char *title = nullptr;
    const char *writer = nullptr;

    if (auto *el = root->FirstChildElement("Series")) series = el->GetText();
    if (auto *el = root->FirstChildElement("Number")) number = el->GetText();
    if (auto *el = root->FirstChildElement("Title"))  title = el->GetText();
    if (auto *el = root->FirstChildElement("Writer")) writer = el->GetText();

    if (series && series[0]) {
        ioTitle = series;
        if (number && number[0]) {
            ioTitle += " #" + std::string(number);
        }
        if (title && title[0]) {
            ioTitle += " - " + std::string(title);
        }
    } else if (title && title[0]) {
        ioTitle = title;
    }

    if (writer && writer[0]) {
        ioAuthor = writer;
    }
}

void ComicZoomPageCache::clear() {
    if (grayBuf) {
        free(grayBuf);
        grayBuf = nullptr;
    }
    width = 0;
    height = 0;
    fitW = 0;
    fitH = 0;
    pageIndex = -1;
}

bool decodeAndRenderComicPageBuffer(
    uint8_t *rawImg,
    size_t rawSize,
    const std::string &entryName,
    Renderer *renderer,
    unsigned char *targetFrameBuffer)
{
    std::string ext = toLowerAscii(entryName.substr(entryName.find_last_of('.') + 1));
    std::vector<uint8_t> gray;
    int imgW = 0, imgH = 0;
    bool ok = false;

    if (ext == "jpg" || ext == "jpeg") {
        ok = decodeJpegEntryAndFreeInput(rawImg, rawSize, EPD_HEIGHT, EPD_WIDTH, gray, imgW, imgH);
    } else if (ext == "png") {
        ok = decodePngEntryAndFreeInput(rawImg, rawSize, EPD_HEIGHT, EPD_WIDTH, gray, imgW, imgH);
    } else {
        free(rawImg);
    }

    if (!ok || gray.empty()) {
        return false;
    }

    enhanceAndDitherComicGray(gray, imgW, imgH);
    blitDitheredToFrameBuffer(renderer, targetFrameBuffer, gray, imgW, imgH);
    return true;
}

bool decodeComicPageToZoomCache(
    uint8_t *rawImg,
    size_t rawSize,
    const std::string &entryName,
    int pageIndex,
    ComicZoomPageCache &outCache)
{
    outCache.clear();
    std::string ext = toLowerAscii(entryName.substr(entryName.find_last_of('.') + 1));

    // Decode up to 2x screen resolution (960 x 1296 = 1.24 MB in PSRAM) for crisp zoomed lettering
    constexpr int MAX_CACHE_W = EPD_HEIGHT * 2;
    constexpr int MAX_CACHE_H = EPD_WIDTH * 2;

    uint8_t *grayBuf = nullptr;
    int imgW = 0, imgH = 0;
    bool ok = false;

    if (ext == "jpg" || ext == "jpeg") {
        ok = decodeJpegEntryToPsram(rawImg, rawSize, MAX_CACHE_W, MAX_CACHE_H, grayBuf, imgW, imgH);
    } else if (ext == "png") {
        ok = decodePngEntryToPsram(rawImg, rawSize, MAX_CACHE_W, MAX_CACHE_H, grayBuf, imgW, imgH);
    } else {
        free(rawImg);
    }

    if (!ok || !grayBuf || imgW <= 0 || imgH <= 0) {
        if (grayBuf) free(grayBuf);
        return false;
    }

    normalizeComicGrayContrast(grayBuf, static_cast<size_t>(imgW) * imgH);

    outCache.grayBuf = grayBuf;
    outCache.width = imgW;
    outCache.height = imgH;
    computeFitDimensions(imgW, imgH, EPD_HEIGHT, EPD_WIDTH, outCache.fitW, outCache.fitH);
    outCache.pageIndex = pageIndex;
    return true;
}

void getComicZoomPanLimits(
    const ComicZoomPageCache &cache,
    int zoomPercent,
    int &outMaxPanX,
    int &outMaxPanY)
{
    if (!cache.grayBuf || cache.fitW <= 0 || cache.fitH <= 0 || zoomPercent <= 100) {
        outMaxPanX = 0;
        outMaxPanY = 0;
        return;
    }
    int virtualW = (cache.fitW * zoomPercent + 50) / 100;
    int virtualH = (cache.fitH * zoomPercent + 50) / 100;
    int dstW = std::min(static_cast<int>(EPD_HEIGHT), virtualW);
    int dstH = std::min(static_cast<int>(EPD_WIDTH), virtualH);
    outMaxPanX = std::max(0, (virtualW - dstW) / 2);
    outMaxPanY = std::max(0, (virtualH - dstH) / 2);
}

bool renderComicZoomCacheView(
    const ComicZoomPageCache &cache,
    int zoomPercent,
    int panX,
    int panY,
    Renderer *renderer,
    unsigned char *targetFrameBuffer)
{
    if (!cache.grayBuf || cache.width <= 0 || cache.height <= 0 || !renderer || !targetFrameBuffer) {
        return false;
    }
    if (zoomPercent < 100) {
        zoomPercent = 100;
    }

    int virtualW = (cache.fitW * zoomPercent + 50) / 100;
    int virtualH = (cache.fitH * zoomPercent + 50) / 100;
    if (virtualW < 1) virtualW = 1;
    if (virtualH < 1) virtualH = 1;

    int dstW = std::min(static_cast<int>(EPD_HEIGHT), virtualW);
    int dstH = std::min(static_cast<int>(EPD_WIDTH), virtualH);

    int maxPanX = std::max(0, (virtualW - dstW) / 2);
    int maxPanY = std::max(0, (virtualH - dstH) / 2);
    int clampedPanX = std::max(-maxPanX, std::min(maxPanX, panX));
    int clampedPanY = std::max(-maxPanY, std::min(maxPanY, panY));

    int viewX = (virtualW - dstW) / 2 + clampedPanX;
    int viewY = (virtualH - dstH) / 2 + clampedPanY;

    std::vector<uint8_t> viewGray(static_cast<size_t>(dstW) * dstH);

    const int FP_SHIFT = 16;
    const int FP_ONE = 1 << FP_SHIFT;
    int64_t srcSpanW = static_cast<int64_t>(std::max(0, cache.width - 1)) << FP_SHIFT;
    int64_t srcSpanH = static_cast<int64_t>(std::max(0, cache.height - 1)) << FP_SHIFT;
    int denomW = std::max(1, virtualW - 1);
    int denomH = std::max(1, virtualH - 1);

    int64_t stepFpX = srcSpanW / denomW;
    int64_t startFpX = (static_cast<int64_t>(viewX) * srcSpanW) / denomW;

    for (int y = 0; y < dstH; ++y) {
        int64_t fpY = (static_cast<int64_t>(viewY + y) * srcSpanH) / denomH;
        int srcY = static_cast<int>(fpY >> FP_SHIFT);
        int yFrac = static_cast<int>(fpY & (FP_ONE - 1));
        if (srcY < 0) { srcY = 0; yFrac = 0; }
        if (srcY >= cache.height) { srcY = cache.height - 1; yFrac = 0; }
        int srcRow = srcY * cache.width;
        int srcRowNext = (srcY + 1 < cache.height) ? (srcY + 1) * cache.width : srcRow;
        uint8_t *dstRow = viewGray.data() + y * dstW;

        int64_t fpX = startFpX;
        for (int x = 0; x < dstW; ++x) {
            int srcX = static_cast<int>(fpX >> FP_SHIFT);
            int xFrac = static_cast<int>(fpX & (FP_ONE - 1));
            if (srcX < 0) { srcX = 0; xFrac = 0; }
            if (srcX >= cache.width) { srcX = cache.width - 1; xFrac = 0; }

            int a = cache.grayBuf[srcRow + srcX];
            int b = (srcX + 1 < cache.width) ? cache.grayBuf[srcRow + srcX + 1] : a;
            int c = cache.grayBuf[srcRowNext + srcX];
            int d = (srcX + 1 < cache.width) ? cache.grayBuf[srcRowNext + srcX + 1] : c;

            int top = a + (((b - a) * xFrac) >> FP_SHIFT);
            int bottom = c + (((d - c) * xFrac) >> FP_SHIFT);
            dstRow[x] = static_cast<uint8_t>(top + (((bottom - top) * yFrac) >> FP_SHIFT));
            fpX += stepFpX;
        }
    }

    ditherComicGray(viewGray, dstW, dstH);
    blitDitheredToFrameBuffer(renderer, targetFrameBuffer, viewGray, dstW, dstH);
    return true;
}

bool isCbzFilename(const std::string &path) {
    std::string base = baseNameOf(path);
    if (base.empty() || base[0] == '.') {
        return false;
    }
    const char *ext = strrchr(base.c_str(), '.');
    return ext && strcasecmp(ext, ".cbz") == 0;
}

bool isCbrFilename(const std::string &path) {
    std::string base = baseNameOf(path);
    if (base.empty() || base[0] == '.') {
        return false;
    }
    const char *ext = strrchr(base.c_str(), '.');
    return ext && strcasecmp(ext, ".cbr") == 0;
}

bool isComicFilename(const std::string &path) {
    return isCbzFilename(path) || isCbrFilename(path);
}

CbzArchive::CbzArchive() : m_zip(new ZipImpl()) {}

CbzArchive::CbzArchive(const std::string &fullPath) : m_zip(new ZipImpl()) {
    open(fullPath);
}

CbzArchive::~CbzArchive() {
    close();
    delete m_zip;
}

void CbzArchive::close() {
    m_pages.clear();
    if (m_zip && m_zip->openFlag) {
        mz_zip_reader_end(&m_zip->archive);
        m_zip->openFlag = false;
    }
}

bool CbzArchive::isOpen() const {
    return m_zip && m_zip->openFlag;
}

bool CbzArchive::open(const std::string &fullPath) {
    close();
    m_path = fullPath;

    memset(&m_zip->archive, 0, sizeof(m_zip->archive));
    if (!mz_zip_reader_init_file(&m_zip->archive, fullPath.c_str(), 0)) {
        ESP_LOGE(TAG, "Failed to open CBZ archive %s: %s",
                 fullPath.c_str(), mz_zip_get_error_string(m_zip->archive.m_last_error));
        return false;
    }
    m_zip->openFlag = true;

    mz_uint numFiles = mz_zip_reader_get_num_files(&m_zip->archive);
    int comicInfoIndex = -1;

    for (mz_uint i = 0; i < numFiles; ++i) {
        if (mz_zip_reader_is_file_a_directory(&m_zip->archive, i)) {
            continue;
        }
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&m_zip->archive, i, &st)) {
            continue;
        }

        std::string entryName(st.m_filename);
        std::string lowerBase = toLowerAscii(baseNameOf(entryName));
        if (lowerBase == "comicinfo.xml") {
            comicInfoIndex = static_cast<int>(i);
            continue;
        }

        if (isComicImageEntry(entryName)) {
            m_pages.push_back({i, std::move(entryName), static_cast<size_t>(st.m_uncomp_size)});
        }
    }

    std::sort(m_pages.begin(), m_pages.end(), [](const PageEntry &a, const PageEntry &b) {
        return naturalLessComicEntry(a.name, b.name);
    });

    m_title = defaultComicTitleFromPath(fullPath);
    m_author = "Comic Book";
    if (comicInfoIndex >= 0) {
        mz_zip_archive_file_stat st;
        if (mz_zip_reader_file_stat(&m_zip->archive, static_cast<mz_uint>(comicInfoIndex), &st) &&
            st.m_uncomp_size > 0 && st.m_uncomp_size <= 256 * 1024) {
            std::vector<char> xmlBuf(st.m_uncomp_size + 1, 0);
            if (mz_zip_reader_extract_to_mem(&m_zip->archive, static_cast<mz_uint>(comicInfoIndex),
                                             xmlBuf.data(), st.m_uncomp_size, 0)) {
                parseComicInfoXml(xmlBuf.data(), st.m_uncomp_size, m_title, m_author);
            }
        }
    }

    ESP_LOGI(TAG, "Opened CBZ '%s' (%zu image pages)", m_title.c_str(), m_pages.size());
    return !m_pages.empty();
}

bool CbzArchive::renderPage(int pageIndex, Renderer *renderer, unsigned char *targetFrameBuffer) {
    if (!isOpen() || !renderer || !targetFrameBuffer) {
        return false;
    }
    if (pageIndex < 0 || pageIndex >= static_cast<int>(m_pages.size())) {
        return false;
    }

    const PageEntry &entry = m_pages[pageIndex];
    if (entry.uncompSize == 0) {
        return false;
    }

    uint8_t *rawImg = allocComicPageBuffer(entry.uncompSize);
    if (!rawImg) {
        ESP_LOGE(TAG, "Failed to allocate %zu bytes for CBZ page %d (%s)",
                 entry.uncompSize, pageIndex, entry.name.c_str());
        return false;
    }

    if (!mz_zip_reader_extract_to_mem(&m_zip->archive, entry.zipIndex, rawImg, entry.uncompSize, 0)) {
        ESP_LOGE(TAG, "Failed to extract CBZ page %d (%s): %s",
                 pageIndex, entry.name.c_str(), mz_zip_get_error_string(m_zip->archive.m_last_error));
        free(rawImg);
        return false;
    }

    return decodeAndRenderComicPageBuffer(rawImg, entry.uncompSize, entry.name, renderer, targetFrameBuffer);
}

bool CbzArchive::loadPageZoomCache(int pageIndex, ComicZoomPageCache &outCache) {
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
    const PageEntry &entry = m_pages[pageIndex];
    if (entry.uncompSize == 0) {
        return false;
    }

    uint8_t *rawImg = allocComicPageBuffer(entry.uncompSize);
    if (!rawImg) {
        ESP_LOGE(TAG, "Failed to allocate %zu bytes for CBZ zoom cache page %d (%s)",
                 entry.uncompSize, pageIndex, entry.name.c_str());
        return false;
    }

    if (!mz_zip_reader_extract_to_mem(&m_zip->archive, entry.zipIndex, rawImg, entry.uncompSize, 0)) {
        ESP_LOGE(TAG, "Failed to extract CBZ zoom cache page %d (%s): %s",
                 pageIndex, entry.name.c_str(), mz_zip_get_error_string(m_zip->archive.m_last_error));
        free(rawImg);
        return false;
    }

    return decodeComicPageToZoomCache(rawImg, entry.uncompSize, entry.name, pageIndex, outCache);
}
