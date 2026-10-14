#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "cbzHandler.h"

class Renderer;

class CbrArchive {
public:
    struct PageEntry {
        int64_t blockPos;
        int archiveOrder;
        std::string name;
        size_t uncompSize;
        bool solid;
    };

    CbrArchive();
    explicit CbrArchive(const std::string &fullPath);
    ~CbrArchive();

    CbrArchive(const CbrArchive &) = delete;
    CbrArchive &operator=(const CbrArchive &) = delete;

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

    // Extracts single page into PSRAM via RARLab UnRAR streaming decompressor (using O(1) direct block
    // seek on non-solid archives or stateful solid stream advancement), decodes JPEG/PNG, scales,
    // enhances paper contrast, Floyd-Steinberg dithers, and centers onto targetFrameBuffer.
    bool renderPage(int pageIndex, Renderer *renderer, unsigned char *targetFrameBuffer);

    // Populates high-resolution PSRAM grayscale cache for fast comic zoom and pan.
    bool loadPageZoomCache(int pageIndex, ComicZoomPageCache &outCache);

    // Extracts raw bytes of a page into caller-allocated buffer of size entry.uncompSize.
    bool extractPageToBuffer(int pageIndex, uint8_t *outBuf, size_t outSize);

private:
    struct RarImpl;
    RarImpl *m_rar = nullptr;
    std::string m_path;
    std::string m_title;
    std::string m_author;
    std::string m_lastError;
    std::vector<std::string> m_parseLog;
    std::vector<PageEntry> m_pages;
};
