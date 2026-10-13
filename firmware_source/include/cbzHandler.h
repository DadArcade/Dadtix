#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class Renderer;

bool isCbzFilename(const std::string &path);

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

private:
    struct ZipImpl;
    ZipImpl *m_zip = nullptr;
    std::string m_path;
    std::string m_title;
    std::string m_author;
    std::vector<PageEntry> m_pages;
};
