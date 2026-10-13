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

static uint8_t *allocLargeBuffer(size_t size) {
    uint8_t *p = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!p) {
        p = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_8BIT));
    }
    return p;
}

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

static bool isComicImageEntry(const std::string &entryName) {
    if (entryName.empty() || entryName.back() == '/' || entryName.back() == '\\') {
        return false;
    }

    // Ignore macOS metadata directories and hidden files (e.g. __MACOSX/, ._001.jpg, .DS_Store)
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

// Natural alphanumeric comparator so both zero-padded (001.jpg..100.jpg) and unpadded
// (Comic_2.jpg < Comic_10.jpg) CBZ archives sort in proper reading sequence.
static bool naturalLessCaseInsensitive(const std::string &a, const std::string &b) {
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

static void extractComicMetadata(
    mz_zip_archive &zip,
    const std::string &archivePath,
    int comicInfoZipIndex,
    std::string &outTitle,
    std::string &outAuthor)
{
    outTitle = stripExtension(baseNameOf(archivePath));
    outAuthor = "Comic Book";

    if (comicInfoZipIndex < 0) {
        return;
    }

    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(&zip, static_cast<mz_uint>(comicInfoZipIndex), &st)) {
        return;
    }
    if (st.m_uncomp_size == 0 || st.m_uncomp_size > 256 * 1024) {
        return;
    }

    std::vector<char> xmlBuf(st.m_uncomp_size + 1, 0);
    if (!mz_zip_reader_extract_to_mem(&zip, static_cast<mz_uint>(comicInfoZipIndex),
                                      xmlBuf.data(), st.m_uncomp_size, 0)) {
        return;
    }

    tinyxml2::XMLDocument doc;
    if (doc.Parse(xmlBuf.data(), st.m_uncomp_size) != tinyxml2::XML_SUCCESS) {
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
        outTitle = series;
        if (number && number[0]) {
            outTitle += " #" + std::string(number);
        }
        if (title && title[0]) {
            outTitle += " - " + std::string(title);
        }
    } else if (title && title[0]) {
        outTitle = title;
    }

    if (writer && writer[0]) {
        outAuthor = writer;
    }
}

static void computeFitDimensions(int srcW, int srcH, int maxW, int maxH, int &outW, int &outH) {
    if (srcW <= 0 || srcH <= 0) {
        outW = maxW;
        outH = maxH;
        return;
    }

    // Fit inside (maxW, maxH) preserving aspect ratio using integer cross-multiplication
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
    if (outH > maxH) outH = maxH;
}

static void resizeGrayscaleBilinear(
    const uint8_t *src, int srcW, int srcH,
    std::vector<uint8_t> &dstOut, int dstW, int dstH)
{
    dstOut.resize(static_cast<size_t>(dstW) * dstH);
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

static bool decodeJpegEntryAndFreeInput(
    uint8_t *jpegBuf, size_t jpegSize,
    int maxW, int maxH,
    std::vector<uint8_t> &outGray, int &outW, int &outH)
{
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

    // Choose largest power-of-two DCT scale factor that keeps decoded dimensions >= (outW, outH)
    uint8_t scale = 0;
    while (scale < 3 &&
           (jd.width >> (scale + 1)) >= outW &&
           (jd.height >> (scale + 1)) >= outH) {
        ++scale;
    }

    int decW = jd.width >> scale;
    int decH = jd.height >> scale;
    size_t decPixels = static_cast<size_t>(decW) * decH;

    uint8_t *decBuf = allocLargeBuffer(decPixels);
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
        outGray.assign(decBuf, decBuf + decPixels);
    } else {
        resizeGrayscaleBilinear(decBuf, decW, decH, outGray, outW, outH);
    }
    free(decBuf);
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

static bool decodePngEntryAndFreeInput(
    uint8_t *pngBuf, size_t pngSize,
    int maxW, int maxH,
    std::vector<uint8_t> &outGray, int &outW, int &outH)
{
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
    uint8_t *rowBuf = allocLargeBuffer(rowPixels);
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
        outGray.assign(rowBuf, rowBuf + rowPixels);
    } else {
        resizeGrayscaleBilinear(rowBuf, outW, srcH, outGray, outW, outH);
    }
    free(rowBuf);
    return true;
}

// Normalizes comic scan black/white levels (1% dark / 98.5% paper clip) so yellowed scanned paper
// renders clean white without peppering speech bubbles, then runs Floyd-Steinberg dithering.
static void enhanceAndDitherComicGray(std::vector<uint8_t> &gray, int width, int height) {
    const size_t total = gray.size();
    if (total == 0 || width <= 0 || height <= 0) return;

    uint32_t hist[256] = {0};
    for (uint8_t v : gray) {
        hist[v]++;
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

bool isCbzFilename(const std::string &path) {
    std::string base = baseNameOf(path);
    if (base.empty() || base[0] == '.') {
        return false;
    }
    const char *ext = strrchr(base.c_str(), '.');
    return ext && strcasecmp(ext, ".cbz") == 0;
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
        return naturalLessCaseInsensitive(a.name, b.name);
    });

    extractComicMetadata(m_zip->archive, fullPath, comicInfoIndex, m_title, m_author);

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

    uint8_t *rawImg = allocLargeBuffer(entry.uncompSize);
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

    std::string ext = toLowerAscii(entry.name.substr(entry.name.find_last_of('.') + 1));
    std::vector<uint8_t> gray;
    int imgW = 0, imgH = 0;
    bool ok = false;

    if (ext == "jpg" || ext == "jpeg") {
        ok = decodeJpegEntryAndFreeInput(rawImg, entry.uncompSize, EPD_HEIGHT, EPD_WIDTH, gray, imgW, imgH);
    } else if (ext == "png") {
        ok = decodePngEntryAndFreeInput(rawImg, entry.uncompSize, EPD_HEIGHT, EPD_WIDTH, gray, imgW, imgH);
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
