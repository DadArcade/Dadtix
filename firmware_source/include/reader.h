#pragma once
#include <string>
#include <list>
#include <vector>
#include <string.h>
#include "htmlParser.h"
#include <Epub.h>
#include "cbzHandler.h"
#include "cbrHandler.h"
#include "pdfHandler.h"
#include "bookHandler.h"
#include "bookMarkMenuHandler.h"
extern "C" {

// Display resolution
#define GLYPH_WIDTH 16
#define GLYPH_HEIGHT 16
#define EPD_WIDTH       648
#define EPD_HEIGHT      480
}


class Reader {
public:
    Reader();
    ~Reader();

    enum class State {
        Reading,
        BookMarkMenu
    };

    void init(Book *book,Renderer* renderer);
    void indexPages(void);
    int renderPages(unsigned char* leftPageFrameBuffer,unsigned char* rightPageFrameBuffer,int pageCacheIndex);
    std::string currentBookPath;
    std::string currentChapterPath;
    std::string cachedChapterPath;
    std::vector<char> cachedChapterHtml;
    void clearChapterCache();
    State state = State::Reading;

    std::vector <int> pageChapterIndex {0,0,0};
    std::vector <int> pageElementIndex {0,0,0};
    std::vector <bool> pageImagePresent {false,false,false};
    bool imagePreviouslyPresent = false;

    //int currentPage = 0;
    int currentChapter = 0;
    //std::vector<int> chapterPageCounts;
    //int chapterCount = 0;
    //int totalPages = 0;
    int getCurrentPageAbsolute(int chapter, int page);
    int getCurrentChapter(int page);
    int getCurrentPageInChapter(int page);
    void nextChapter(void);
    void prevChapter(void);
    void nextPage(void);
    void prevPage(void);
    void openPage(void);
    void middleButtonAction();
    void rightButtonAction();
    void leftButtonAction();
    void upButtonAction();
    void downButtonAction();
    void rightPageAction();
    void leftPageAction();

    void doWhileListening(std::function<void()> func);
    void addBookMark();
    bool checkBookMark();
    int findCurrentBookMarkIndex();

    Epub *epub = nullptr;
    CbzArchive *cbz = nullptr;
    CbrArchive *cbr = nullptr;
    PdfArchive *pdf = nullptr;
    Renderer* renderer = nullptr;
    BookMarkMenuHandler *bookMarkMenuHandler = nullptr;
    Book *book = nullptr;

    unsigned char* leftPageFrameBuffer;
    unsigned char* rightPageFrameBuffer;
    unsigned char* leftPageFrameBufferNext;
    unsigned char* rightPageFrameBufferNext;
    unsigned char* leftPageFrameBufferPrevious;
    unsigned char* rightPageFrameBufferPrevious;
    //unsigned char* activeFrameBuffer;

    static constexpr int COMIC_ZOOM_MIN = 100;
    static constexpr int COMIC_ZOOM_MAX = 250;
    static constexpr int COMIC_ZOOM_STEP = 50;
    static constexpr int COMIC_PAN_STEP_X = 120;
    static constexpr int COMIC_PAN_STEP_Y = 160;

    int comicZoomPercent = COMIC_ZOOM_MIN;
    int comicPanX = 0;
    int comicPanY = 0;

    bool isComicZoomed() const;
    void resetComicZoom(bool freeCache = true);
    bool zoomComicIn();
    bool zoomComicOut();
    bool panComic(int deltaX, int deltaY);

private:
    ComicZoomPageCache leftZoomCache;
    ComicZoomPageCache rightZoomCache;

    bool hasOpenComic() const;
    bool ensureCurrentSpreadZoomCache();
    void getCurrentSpreadPanLimits(int zoomPercent, int &outMaxPanX, int &outMaxPanY) const;
    void renderAndDisplayComicZoomSpread();
    void renderComicSpread(unsigned char* leftPageFrameBuffer, unsigned char* rightPageFrameBuffer, int pageCacheIndex);
};

int countBlackPixels(unsigned char* framebuffer);
inline uint8_t pop8(uint8_t v);
