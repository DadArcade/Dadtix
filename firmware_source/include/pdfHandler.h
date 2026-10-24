#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "cbzHandler.h"

class Renderer;

bool isPdfFilename(const std::string &path);

class PdfArchive {
public:
    enum class ImageFormat {
        Jpeg,
        FlateJpeg,
        FlateRaw,
        Png,
        Jbig2,
        ContentStream,
        Unknown
    };

    struct PageEntry {
        int pageObjId = 0;
        int imgObjId = 0;
        int64_t streamOffset = 0;
        size_t streamLen = 0;
        ImageFormat format = ImageFormat::Jpeg;
        int width = 0;
        int height = 0;
        int bitsPerComponent = 8;
        int colorComponents = 3; // 1 = gray, 3 = RGB, 4 = CMYK
        int predictor = 1;       // 1 = none, 2 = TIFF, 10..15 = PNG
        int predictorColumns = 0;
        int rotate = 0;
        bool resolved = false;
    };

    PdfArchive();
    explicit PdfArchive(const std::string &fullPath);
    ~PdfArchive();

    PdfArchive(const PdfArchive &) = delete;
    PdfArchive &operator=(const PdfArchive &) = delete;

    bool open(const std::string &fullPath);
    void close();
    bool isOpen() const;

    int getPageCount() const { return static_cast<int>(m_pages.size()); }
    const std::string &getTitle() const { return m_title; }
    const std::string &getAuthor() const { return m_author; }
    const std::string &getPath() const { return m_path; }
    const std::vector<PageEntry> &getPages() const { return m_pages; }
    const std::string &getLastError() const { return m_lastError; }
    const std::vector<std::string> &getParseLog() const { return m_parseLog; }

    // Extracts page image, scales to fit (EPD_HEIGHT, EPD_WIDTH), enhances paper contrast,
    // applies Floyd-Steinberg dithering, and centers onto targetFrameBuffer.
    bool renderPage(int pageIndex, Renderer *renderer, unsigned char *targetFrameBuffer);

    // Populates high-resolution PSRAM grayscale cache for fast comic zoom and pan.
    bool loadPageZoomCache(int pageIndex, ComicZoomPageCache &outCache);

    // Ensures lazy page metadata (streamOffset, format, dimensions) is resolved for pageIndex.
    bool ensurePageResolved(int pageIndex);

private:
    struct PdfImpl;
    PdfImpl *m_impl = nullptr;
    std::string m_path;
    std::string m_title;
    std::string m_author;
    std::string m_lastError;
    std::vector<std::string> m_parseLog;
    std::vector<PageEntry> m_pages;

    bool parseDocument();
};
