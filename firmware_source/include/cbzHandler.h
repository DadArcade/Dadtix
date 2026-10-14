#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class Renderer;

bool isCbzFilename(const std::string &path);
bool isCbrFilename(const std::string &path);
bool isComicFilename(const std::string &path);

// Shared comic book archive helpers used by both CBZ (ZIP) and CBR (RAR) handlers
struct ComicZoomPageCache {
    uint8_t *grayBuf = nullptr;
    int width = 0;
    int height = 0;
    int fitW = 0;
    int fitH = 0;
    int pageIndex = -1;

    ComicZoomPageCache() = default;
    ~ComicZoomPageCache() { clear(); }

    ComicZoomPageCache(const ComicZoomPageCache &) = delete;
    ComicZoomPageCache &operator=(const ComicZoomPageCache &) = delete;

    void clear();
    bool isValidFor(int idx) const {
        return grayBuf != nullptr && width > 0 && height > 0 && pageIndex == idx;
    }
};

uint8_t *allocComicPageBuffer(size_t size);
bool isComicImageEntry(const std::string &entryName);
bool naturalLessComicEntry(const std::string &a, const std::string &b);
std::string defaultComicTitleFromPath(const std::string &archivePath);
void parseComicInfoXml(const char *xmlData, size_t xmlLen, std::string &ioTitle, std::string &ioAuthor);
bool decodeAndRenderComicPageBuffer(
    uint8_t *rawImg,
    size_t rawSize,
    const std::string &entryName,
    Renderer *renderer,
    unsigned char *targetFrameBuffer);
bool decodeComicPageToZoomCache(
    uint8_t *rawImg,
    size_t rawSize,
    const std::string &entryName,
    int pageIndex,
    ComicZoomPageCache &outCache);
void getComicZoomPanLimits(
    const ComicZoomPageCache &cache,
    int zoomPercent,
    int &outMaxPanX,
    int &outMaxPanY);
bool renderComicZoomCacheView(
    const ComicZoomPageCache &cache,
    int zoomPercent,
    int panX,
    int panY,
    Renderer *renderer,
    unsigned char *targetFrameBuffer);

class CbzArchive {
public:
    struct PageEntry {
        uint32_t zipIndex;
        std::string name;
        size_t uncompSize;
    };

    CbzArchive();
    explicit CbzArchive(const std::string &fullPath);
    ~CbzArchive();

    CbzArchive(const CbzArchive &) = delete;
    CbzArchive &operator=(const CbzArchive &) = delete;

    bool open(const std::string &fullPath);
    void close();
    bool isOpen() const;

    int getPageCount() const { return static_cast<int>(m_pages.size()); }
    const std::string &getTitle() const { return m_title; }
    const std::string &getAuthor() const { return m_author; }
    const std::string &getPath() const { return m_path; }
    const std::vector<PageEntry> &getPages() const { return m_pages; }

    // Extracts, decodes, scales to fit within (EPD_HEIGHT, EPD_WIDTH) preserving aspect ratio,
    // enhances comic ink/paper contrast, applies Floyd-Steinberg dithering, and centers onto targetFrameBuffer.
    bool renderPage(int pageIndex, Renderer *renderer, unsigned char *targetFrameBuffer);

    // Populates high-resolution PSRAM grayscale cache for fast comic zoom and pan.
    bool loadPageZoomCache(int pageIndex, ComicZoomPageCache &outCache);

private:
    struct ZipImpl;
    ZipImpl *m_zip = nullptr;
    std::string m_path;
    std::string m_title;
    std::string m_author;
    std::vector<PageEntry> m_pages;
};
