#include "reader.h"
#include "esp_log.h"
#include <stdio.h>
#include <dirent.h>
#include <string>
#include <vector>
#include <algorithm>
#include "driver/adc.h"
#include <functional>
#include "device.h"
#include "esp_timer.h"
#include "menuHandler.h"

static const char *TAG = "reader";

Reader::Reader()
{
    leftPageFrameBuffer = (unsigned char*)calloc(EPD_WIDTH * EPD_HEIGHT / 8,sizeof(unsigned char));
    leftPageFrameBufferNext = (unsigned char*)calloc(EPD_WIDTH * EPD_HEIGHT / 8,sizeof(unsigned char));
    leftPageFrameBufferPrevious = (unsigned char*)calloc(EPD_WIDTH * EPD_HEIGHT / 8,sizeof(unsigned char));
    rightPageFrameBuffer = (unsigned char*)calloc(EPD_WIDTH * EPD_HEIGHT / 8,sizeof(unsigned char));
    rightPageFrameBufferNext = (unsigned char*)calloc(EPD_WIDTH * EPD_HEIGHT / 8,sizeof(unsigned char));
    rightPageFrameBufferPrevious = (unsigned char*)calloc(EPD_WIDTH * EPD_HEIGHT / 8,sizeof(unsigned char));
}

Reader::~Reader()
{
    free(leftPageFrameBuffer);
    free(leftPageFrameBufferNext);
    free(leftPageFrameBufferPrevious);
    free(rightPageFrameBuffer);
    free(rightPageFrameBufferNext);
    free(rightPageFrameBufferPrevious);
    clearChapterCache();
    resetComicZoom(true);
    if (epub) {
        delete epub;
        epub = nullptr;
    }
    if (cbz) {
        delete cbz;
        cbz = nullptr;
    }
    if (cbr) {
        delete cbr;
        cbr = nullptr;
    }
    if (pdf) {
        delete pdf;
        pdf = nullptr;
    }
}

void Reader::clearChapterCache()
{
    cachedChapterPath.clear();
    cachedChapterHtml.clear();
    cachedChapterHtml.shrink_to_fit();
}

inline uint8_t pop8(uint8_t v) {
    v = v - ((v >> 1) & 0x55);
    v = (v & 0x33) + ((v >> 2) & 0x33);
    return (((v + (v >> 4)) & 0x0F) * 0x01);
}

int countBlackPixels(unsigned char* framebuffer)
{
    size_t len = EPD_HEIGHT*EPD_WIDTH/8;
    int sum = 0;
    for (size_t i = 0; i < len; i++)
        sum += pop8(framebuffer[i]);

    return sum;
}

void Reader::init(Book *book,Renderer* renderer)
{
    this->renderer = renderer;
    this->book = book;
    this->currentBookPath = book->path;
    this->currentChapter = 0;

    clearChapterCache();
    resetComicZoom(true);
    if (this->epub) {
        delete this->epub;
        this->epub = nullptr;
    }
    if (this->cbz) {
        delete this->cbz;
        this->cbz = nullptr;
    }
    if (this->cbr) {
        delete this->cbr;
        this->cbr = nullptr;
    }
    if (this->pdf) {
        delete this->pdf;
        this->pdf = nullptr;
    }

    ESP_LOGI(TAG, "book path: %s", this->currentBookPath.c_str());
    if (book->isCbz()) {
        this->cbz = new CbzArchive(std::string("/sdcard/") + this->currentBookPath);
        if (this->cbz->isOpen()) {
            this->book->totalPageCount = this->cbz->getPageCount();
            if (this->book->currentPage < 0 || this->book->currentPage >= this->book->totalPageCount) {
                this->book->currentPage = 0;
                this->book->currentPageElementIndex = 0;
                this->book->currentPageChapterIndex = 0;
            }
            this->book->currentPage &= ~1;
            ESP_LOGI(TAG, "CBZ loaded: '%s' (%d pages)",
                     this->cbz->getTitle().c_str(), this->book->totalPageCount);
        }
        return;
    }
    if (book->isCbr()) {
        this->cbr = new CbrArchive(std::string("/sdcard/") + this->currentBookPath);
        if (this->cbr->isOpen()) {
            this->book->totalPageCount = this->cbr->getPageCount();
            if (this->book->currentPage < 0 || this->book->currentPage >= this->book->totalPageCount) {
                this->book->currentPage = 0;
                this->book->currentPageElementIndex = 0;
                this->book->currentPageChapterIndex = 0;
            }
            this->book->currentPage &= ~1;
            ESP_LOGI(TAG, "CBR loaded: '%s' (%d pages)",
                     this->cbr->getTitle().c_str(), this->book->totalPageCount);
        }
        return;
    }
    if (book->isPdf()) {
        this->pdf = new PdfArchive(std::string("/sdcard/") + this->currentBookPath);
        if (this->pdf->isOpen() && this->pdf->getPageCount() > 0) {
            this->book->totalPageCount = this->pdf->getPageCount();
            this->book->badParse = false;
            this->book->parseError.clear();
            for (const std::string &entry : this->pdf->getParseLog()) {
                if (this->book->parseLog.size() < 40 &&
                    std::find(this->book->parseLog.begin(), this->book->parseLog.end(), entry) == this->book->parseLog.end()) {
                    this->book->parseLog.push_back(entry);
                }
            }
            if (this->book->currentPage < 0 || this->book->currentPage >= this->book->totalPageCount) {
                this->book->currentPage = 0;
                this->book->currentPageElementIndex = 0;
                this->book->currentPageChapterIndex = 0;
            }
            this->book->currentPage &= ~1;
            ESP_LOGI(TAG, "PDF loaded: '%s' (%d pages, currentPage=%d)",
                     this->pdf->getTitle().c_str(), this->book->totalPageCount, this->book->currentPage);
            if (Device::getInstance().bookHandler) {
                Device::getInstance().bookHandler->saveBook(this->book);
            }
        } else {
            std::string errMsg = "reader_open_fail=" + (!this->pdf->getLastError().empty() ? this->pdf->getLastError() : "0 pages");
            if (std::find(this->book->parseLog.begin(), this->book->parseLog.end(), errMsg) == this->book->parseLog.end()) {
                this->book->parseLog.push_back(errMsg);
            }
            if (Device::getInstance().bookHandler) {
                Device::getInstance().bookHandler->saveBook(this->book);
            }
        }
        return;
    }

    this->epub = new Epub(std::string("/sdcard/") + this->currentBookPath);
    if (epub->load())
    {
        ESP_LOGI(TAG, "book title: %s", (epub->get_title()).c_str());
        ESP_LOGI(TAG, "first chapter: %s", (epub->get_spine_item(0)).c_str());
    }
}

int Reader::getCurrentPageAbsolute(int chapter, int page)
{
    int pageCounter = 0;
    for(int i = 0;i<chapter;i++)
    {
        pageCounter+= book->chapterPageCounts[i];
    }
    pageCounter+=page;
    return pageCounter;
}

int Reader::getCurrentChapter(int page)
{
    if (!this->epub) return 0;
    int pageCounter = page;
    for(int i = 0;i<this->epub->get_spine_items_count();i++)
    {
        pageCounter-= book->chapterPageCounts[i];
        if(pageCounter<0) return i;
    }
    return 0;
}

int Reader::getCurrentPageInChapter(int page)
{
    if (!this->epub) return 0;
    int pageCounter = page;
    for(int i = 0;i<this->epub->get_spine_items_count();i++)
    {
        if(pageCounter<book->chapterPageCounts[i]) return pageCounter;
        pageCounter-= book->chapterPageCounts[i];
    }
    return 0;
}

void Reader::doWhileListening(std::function<void()> func)
{
    Device::getInstance().clearButtonLatches();
    Device::getInstance().setLatchTimeOut(100000);

    // Run the provided function
    func();

    // Then check latched button states
    if(Device::getInstance().buttonLatchedStates[MIDDLE_BUTTON]) { middleButtonAction(); return; }
    if(Device::getInstance().buttonLatchedStates[PAGE_RIGHT_BUTTON]) { rightPageAction(); return; }
    if(Device::getInstance().buttonLatchedStates[PAGE_LEFT_BUTTON])  { leftPageAction(); return; }
    if(Device::getInstance().buttonLatchedStates[ARROW_DOWN_BUTTON]) { downButtonAction(); return; }
    if(Device::getInstance().buttonLatchedStates[ARROW_UP_BUTTON])   { upButtonAction(); return; }
    Device::getInstance().clearButtonLatches();
    Device::getInstance().setLatchTimeOut(100000);
}

bool Reader::hasOpenComic() const
{
    return (this->cbz && this->cbz->isOpen()) ||
           (this->cbr && this->cbr->isOpen()) ||
           (this->pdf && this->pdf->isOpen());
}

bool Reader::isComicZoomed() const
{
    return hasOpenComic() && comicZoomPercent > COMIC_ZOOM_MIN;
}

void Reader::resetComicZoom(bool freeCache)
{
    comicZoomPercent = COMIC_ZOOM_MIN;
    comicPanX = 0;
    comicPanY = 0;
    if (freeCache) {
        leftZoomCache.clear();
        rightZoomCache.clear();
    }
}

bool Reader::ensureCurrentSpreadZoomCache()
{
    if (!hasOpenComic() || !book) {
        return false;
    }

    int leftIdx = book->currentPage;
    int rightIdx = book->currentPage + 1;
    int totalPages = (this->cbz && this->cbz->isOpen())
        ? this->cbz->getPageCount()
        : ((this->cbr && this->cbr->isOpen())
            ? this->cbr->getPageCount()
            : ((this->pdf && this->pdf->isOpen()) ? this->pdf->getPageCount() : 0));

    bool anyOk = false;
    if (leftIdx >= 0 && leftIdx < totalPages) {
        if (this->cbz && this->cbz->isOpen()) {
            anyOk |= this->cbz->loadPageZoomCache(leftIdx, leftZoomCache);
        } else if (this->cbr && this->cbr->isOpen()) {
            anyOk |= this->cbr->loadPageZoomCache(leftIdx, leftZoomCache);
        } else if (this->pdf && this->pdf->isOpen()) {
            anyOk |= this->pdf->loadPageZoomCache(leftIdx, leftZoomCache);
        }
    } else {
        leftZoomCache.clear();
    }

    if (rightIdx >= 0 && rightIdx < totalPages) {
        if (this->cbz && this->cbz->isOpen()) {
            anyOk |= this->cbz->loadPageZoomCache(rightIdx, rightZoomCache);
        } else if (this->cbr && this->cbr->isOpen()) {
            anyOk |= this->cbr->loadPageZoomCache(rightIdx, rightZoomCache);
        } else if (this->pdf && this->pdf->isOpen()) {
            anyOk |= this->pdf->loadPageZoomCache(rightIdx, rightZoomCache);
        }
    } else {
        rightZoomCache.clear();
    }

    return anyOk;
}

void Reader::getCurrentSpreadPanLimits(int zoomPercent, int &outMaxPanX, int &outMaxPanY) const
{
    int leftMaxX = 0, leftMaxY = 0;
    int rightMaxX = 0, rightMaxY = 0;
    if (leftZoomCache.grayBuf) {
        getComicZoomPanLimits(leftZoomCache, zoomPercent, leftMaxX, leftMaxY);
    }
    if (rightZoomCache.grayBuf) {
        getComicZoomPanLimits(rightZoomCache, zoomPercent, rightMaxX, rightMaxY);
    }
    outMaxPanX = std::max(leftMaxX, rightMaxX);
    outMaxPanY = std::max(leftMaxY, rightMaxY);
}

void Reader::renderAndDisplayComicZoomSpread()
{
    renderer->clearScreenBuffer(leftPageFrameBuffer);
    renderer->clearScreenBuffer(rightPageFrameBuffer);

    int leftIdx = book->currentPage;
    int rightIdx = book->currentPage + 1;
    int totalPages = (this->cbz && this->cbz->isOpen())
        ? this->cbz->getPageCount()
        : ((this->cbr && this->cbr->isOpen())
            ? this->cbr->getPageCount()
            : ((this->pdf && this->pdf->isOpen()) ? this->pdf->getPageCount() : 0));

    if (leftIdx >= 0 && leftIdx < totalPages) {
        if (leftZoomCache.isValidFor(leftIdx)) {
            renderComicZoomCacheView(leftZoomCache, comicZoomPercent, comicPanX, comicPanY, renderer, leftPageFrameBuffer);
        } else if (this->cbz && this->cbz->isOpen()) {
            this->cbz->renderPage(leftIdx, renderer, leftPageFrameBuffer);
        } else if (this->cbr && this->cbr->isOpen()) {
            this->cbr->renderPage(leftIdx, renderer, leftPageFrameBuffer);
        } else if (this->pdf && this->pdf->isOpen()) {
            this->pdf->renderPage(leftIdx, renderer, leftPageFrameBuffer);
        }
        renderer->drawPageOverlay(leftPageFrameBuffer, leftIdx + 1, book->totalPageCount);
    }

    if (rightIdx >= 0 && rightIdx < totalPages) {
        if (rightZoomCache.isValidFor(rightIdx)) {
            renderComicZoomCacheView(rightZoomCache, comicZoomPercent, comicPanX, comicPanY, renderer, rightPageFrameBuffer);
        } else if (this->cbz && this->cbz->isOpen()) {
            this->cbz->renderPage(rightIdx, renderer, rightPageFrameBuffer);
        } else if (this->cbr && this->cbr->isOpen()) {
            this->cbr->renderPage(rightIdx, renderer, rightPageFrameBuffer);
        } else if (this->pdf && this->pdf->isOpen()) {
            this->pdf->renderPage(rightIdx, renderer, rightPageFrameBuffer);
        }
        renderer->drawPageOverlay(rightPageFrameBuffer, rightIdx + 1, book->totalPageCount);
    }

    if (checkBookMark()) {
        renderer->drawBookMark(rightPageFrameBuffer, false);
    }
    renderer->drawBattery(rightPageFrameBuffer, Device::getInstance().getBatteryPercentage());
    renderer->epd.DisplayPictureBoth(leftPageFrameBuffer, rightPageFrameBuffer);
}

bool Reader::zoomComicIn()
{
    if (!hasOpenComic() || comicZoomPercent >= COMIC_ZOOM_MAX) {
        return false;
    }
    if (!ensureCurrentSpreadZoomCache()) {
        return false;
    }

    int oldZoom = comicZoomPercent;
    int oldMaxPanX = 0, oldMaxPanY = 0;
    getCurrentSpreadPanLimits(oldZoom, oldMaxPanX, oldMaxPanY);

    comicZoomPercent = std::min(COMIC_ZOOM_MAX, comicZoomPercent + COMIC_ZOOM_STEP);

    int maxPanX = 0, maxPanY = 0;
    getCurrentSpreadPanLimits(comicZoomPercent, maxPanX, maxPanY);
    if (oldZoom <= COMIC_ZOOM_MIN) {
        comicPanX = -maxPanX;
        comicPanY = -maxPanY;
    } else {
        int scaledPanX = (oldMaxPanX > 0)
            ? (comicPanX * maxPanX) / oldMaxPanX
            : (comicPanX * comicZoomPercent) / std::max(100, oldZoom);
        int scaledPanY = (oldMaxPanY > 0)
            ? (comicPanY * maxPanY) / oldMaxPanY
            : (comicPanY * comicZoomPercent) / std::max(100, oldZoom);
        comicPanX = std::max(-maxPanX, std::min(maxPanX, scaledPanX));
        comicPanY = std::max(-maxPanY, std::min(maxPanY, scaledPanY));
    }

    renderAndDisplayComicZoomSpread();
    return true;
}

bool Reader::zoomComicOut()
{
    if (!hasOpenComic() || comicZoomPercent <= COMIC_ZOOM_MIN) {
        return false;
    }
    if (!ensureCurrentSpreadZoomCache()) {
        return false;
    }

    int oldZoom = comicZoomPercent;
    int oldMaxPanX = 0, oldMaxPanY = 0;
    getCurrentSpreadPanLimits(oldZoom, oldMaxPanX, oldMaxPanY);

    comicZoomPercent = std::max(COMIC_ZOOM_MIN, comicZoomPercent - COMIC_ZOOM_STEP);

    if (comicZoomPercent <= COMIC_ZOOM_MIN) {
        comicPanX = 0;
        comicPanY = 0;
    } else {
        int maxPanX = 0, maxPanY = 0;
        getCurrentSpreadPanLimits(comicZoomPercent, maxPanX, maxPanY);
        int scaledPanX = (oldMaxPanX > 0)
            ? (comicPanX * maxPanX) / oldMaxPanX
            : (comicPanX * comicZoomPercent) / std::max(100, oldZoom);
        int scaledPanY = (oldMaxPanY > 0)
            ? (comicPanY * maxPanY) / oldMaxPanY
            : (comicPanY * comicZoomPercent) / std::max(100, oldZoom);
        comicPanX = std::max(-maxPanX, std::min(maxPanX, scaledPanX));
        comicPanY = std::max(-maxPanY, std::min(maxPanY, scaledPanY));
    }

    renderAndDisplayComicZoomSpread();
    return true;
}

bool Reader::panComic(int deltaX, int deltaY)
{
    if (!isComicZoomed()) {
        return false;
    }
    if (!ensureCurrentSpreadZoomCache()) {
        return false;
    }

    int maxPanX = 0, maxPanY = 0;
    getCurrentSpreadPanLimits(comicZoomPercent, maxPanX, maxPanY);

    int newPanX = std::max(-maxPanX, std::min(maxPanX, comicPanX + deltaX));
    int newPanY = std::max(-maxPanY, std::min(maxPanY, comicPanY + deltaY));
    if (newPanX == comicPanX && newPanY == comicPanY) {
        return false;
    }

    comicPanX = newPanX;
    comicPanY = newPanY;
    renderAndDisplayComicZoomSpread();
    return true;
}

void Reader::renderComicSpread(unsigned char* leftPageFrameBuffer, unsigned char* rightPageFrameBuffer, int pageCacheIndex)
{
    renderer->clearScreenBuffer(leftPageFrameBuffer);
    renderer->clearScreenBuffer(rightPageFrameBuffer);

    int totalPages = (this->cbz && this->cbz->isOpen())
        ? this->cbz->getPageCount()
        : ((this->cbr && this->cbr->isOpen())
            ? this->cbr->getPageCount()
            : ((this->pdf && this->pdf->isOpen()) ? this->pdf->getPageCount() : 0));

    if (totalPages > 0 && pageCacheIndex == 1) {
        if (book->totalPageCount != totalPages) {
            book->totalPageCount = totalPages;
        }
        if (book->currentPage < 0 || book->currentPage >= totalPages) {
            book->currentPage = 0;
            book->currentPageElementIndex = 0;
            book->currentPageChapterIndex = 0;
        }
    }

    int leftIdx = book->currentPage;
    int rightIdx = book->currentPage + 1;

    bool useZoomView = (pageCacheIndex == 1 && comicZoomPercent > COMIC_ZOOM_MIN);

    auto drawComicPage = [this, useZoomView, pageCacheIndex](int pageIdx, unsigned char *fb, ComicZoomPageCache &cacheSlot) {
        if (useZoomView && cacheSlot.isValidFor(pageIdx)) {
            renderComicZoomCacheView(cacheSlot, this->comicZoomPercent, this->comicPanX, this->comicPanY, this->renderer, fb);
            return;
        }
        if (this->cbz && this->cbz->isOpen()) {
            this->cbz->renderPage(pageIdx, this->renderer, fb);
        } else if (this->cbr && this->cbr->isOpen()) {
            this->cbr->renderPage(pageIdx, this->renderer, fb);
        } else if (this->pdf && this->pdf->isOpen()) {
            size_t logBefore = this->pdf->getParseLog().size();
            this->pdf->renderPage(pageIdx, this->renderer, fb);
            if (this->book && this->pdf->getParseLog().size() > logBefore) {
                bool addedAny = false;
                const auto &pdfLog = this->pdf->getParseLog();
                for (size_t i = logBefore; i < pdfLog.size(); ++i) {
                    if (this->book->parseLog.size() < 40 &&
                        std::find(this->book->parseLog.begin(), this->book->parseLog.end(), pdfLog[i]) == this->book->parseLog.end()) {
                        this->book->parseLog.push_back(pdfLog[i]);
                        addedAny = true;
                    }
                }
                if (addedAny && pageCacheIndex == 1 && this->book->currentPage == 0 && Device::getInstance().bookHandler) {
                    Device::getInstance().bookHandler->saveBook(this->book);
                }
            }
        }
    };

    if (leftIdx >= 0 && leftIdx < totalPages) {
        drawComicPage(leftIdx, leftPageFrameBuffer, leftZoomCache);
        this->renderer->drawPageOverlay(leftPageFrameBuffer, leftIdx + 1, book->totalPageCount);
    }
    if (rightIdx >= 0 && rightIdx < totalPages) {
        drawComicPage(rightIdx, rightPageFrameBuffer, rightZoomCache);
        this->renderer->drawPageOverlay(rightPageFrameBuffer, rightIdx + 1, book->totalPageCount);
    }

    pageElementIndex[pageCacheIndex] = book->currentPage;
    pageChapterIndex[pageCacheIndex] = 0;
    pageImagePresent[pageCacheIndex] = true;

    if (checkBookMark()) {
        renderer->drawBookMark(rightPageFrameBuffer, false);
    }
    if (book->currentPage == 0 && pageCacheIndex == 1) {
        this->renderer->epd.forceRefresh();
    }
}

int Reader::renderPages(unsigned char* leftPageFrameBuffer,unsigned char* rightPageFrameBuffer,int pageCacheIndex)
{
    ESP_LOGI(TAG, "rendering page %d", book->currentPage);
    if (hasOpenComic()) {
        renderComicSpread(leftPageFrameBuffer, rightPageFrameBuffer, pageCacheIndex);
        return 0;
    }

    currentChapter = getCurrentChapter(book->currentPage);
    int currentPageInChapter = getCurrentPageInChapter(book->currentPage);
    std::string currentChapterPath = epub->get_spine_item(this->currentChapter);
    ESP_LOGI(TAG, "render Chapter: %s", currentChapterPath.c_str());

    if (currentChapterPath != cachedChapterPath || cachedChapterHtml.empty())
    {
        size_t htmlSize = 0;
        uint8_t *rawHtml = epub->get_item_contents(currentChapterPath, &htmlSize);
        if (rawHtml)
        {
            cachedChapterHtml.assign(reinterpret_cast<char*>(rawHtml), reinterpret_cast<char*>(rawHtml) + htmlSize);
            cachedChapterHtml.push_back('\0');
            cachedChapterPath = currentChapterPath;
            free(rawHtml);
            ESP_LOGI(TAG, "Cached chapter %s (%zu bytes)", cachedChapterPath.c_str(), htmlSize);
        }
        else
        {
            clearChapterCache();
        }
    }

    if (!cachedChapterHtml.empty())
    {
        char *html = cachedChapterHtml.data();
        renderer->clearScreenBuffer(leftPageFrameBuffer);
        renderer->clearScreenBuffer(rightPageFrameBuffer);
        HtmlParser *parser = nullptr;
        parser = new HtmlParser(html, strlen(html), currentChapterPath,this->renderer,currentPageInChapter,this->epub,leftPageFrameBuffer,rightPageFrameBuffer);
        parser->cachedImages = book->cachedImages;
        parser->parse();
        pageElementIndex[pageCacheIndex] = parser->currentElementIndex;
        pageChapterIndex[pageCacheIndex] = currentChapter;
        pageImagePresent[pageCacheIndex] = parser->imagePresentOnPage;

        delete parser;
        this->renderer->drawPageOverlay(leftPageFrameBuffer,book->currentPage+1,book->totalPageCount);
        this->renderer->drawPageOverlay(rightPageFrameBuffer,book->currentPage+2,book->totalPageCount);
        // if (Device::getInstance().deviceSettings.displayBattery) {
        //     int batteryLevel = Device::getInstance().getBatteryPercentage();;
        //     this->renderer->drawBattery(rightPageFrameBuffer,batteryLevel);
        // }
        if(checkBookMark()) renderer->drawBookMark(rightPageFrameBuffer,false);
        if(book->currentPage==0) this->renderer->epd.forceRefresh();
    }
    return 0;
}

void Reader::openPage()
{
    resetComicZoom(true);
    Device::getInstance().clearButtonLatches();
    Device::getInstance().setLatchTimeOut(100000);
    renderPages(leftPageFrameBuffer, rightPageFrameBuffer,1);
    if((imagePreviouslyPresent!=pageImagePresent[1]) || pageImagePresent[1]) renderer->epd.forceRefresh();
    imagePreviouslyPresent=pageImagePresent[1];
    renderer->drawBattery(rightPageFrameBuffer,Device::getInstance().getBatteryPercentage());
    this->renderer->epd.DisplayPictureBoth(
        leftPageFrameBuffer,
        rightPageFrameBuffer,
        [this]() {
            if (!hasOpenComic() || book->currentPage + 2 < book->totalPageCount) {
                book->currentPage += 2;
                renderPages(leftPageFrameBufferNext, rightPageFrameBufferNext,2);
                book->currentPage -= 2;
            }

            if (book->currentPage >= 2) {
                book->currentPage -= 2;
                renderPages(leftPageFrameBufferPrevious, rightPageFrameBufferPrevious,0);
                book->currentPage += 2;
            }
        }
    );
    book->currentPageElementIndex = pageElementIndex[1]; //store what chapter and html element the current page is at
    book->currentPageChapterIndex = pageChapterIndex[1];
    ESP_LOGI(TAG, "image present array: %d %d %d",
         static_cast<int>(pageImagePresent[0]),
         static_cast<int>(pageImagePresent[1]),
         static_cast<int>(pageImagePresent[2]));
    
    if(Device::getInstance().buttonLatchedStates[MIDDLE_BUTTON]) {middleButtonAction(); return;}
    if(Device::getInstance().buttonLatchedStates[PAGE_RIGHT_BUTTON]) {rightPageAction(); return;}
    if(Device::getInstance().buttonLatchedStates[PAGE_LEFT_BUTTON]) {leftPageAction(); return;}
    if(Device::getInstance().buttonLatchedStates[ARROW_DOWN_BUTTON]) {downButtonAction(); return;}
    if(Device::getInstance().buttonLatchedStates[ARROW_UP_BUTTON]) {upButtonAction(); return;}
}

void Reader::nextChapter()
{
    resetComicZoom(true);
    if (hasOpenComic()) {
        int nextP = book->currentPage + 10;
        int maxStart = (book->totalPageCount >= 2) ? ((book->totalPageCount - 1) & ~1) : 0;
        if (nextP > maxStart) nextP = maxStart;
        book->currentPage = nextP;
        openPage();
        return;
    }
    currentChapter = getCurrentChapter(book->currentPage);
    if(currentChapter<book->chapterPageCounts.size()-1) currentChapter++;
    book->currentPage = getCurrentPageAbsolute(currentChapter,0);
    openPage();
}

void Reader::prevChapter()
{
    resetComicZoom(true);
    if (hasOpenComic()) {
        int prevP = book->currentPage - 10;
        if (prevP < 0) prevP = 0;
        book->currentPage = prevP;
        openPage();
        return;
    }
    currentChapter = getCurrentChapter(book->currentPage);
    if(currentChapter>0 && getCurrentPageInChapter(book->currentPage)==0) currentChapter--;
    book->currentPage = getCurrentPageAbsolute(currentChapter,0);
    openPage();
}

void Reader::nextPage()
{
    resetComicZoom(true);
    Device::getInstance().clearButtonLatches();
    Device::getInstance().setLatchTimeOut(100000);
    if(book->currentPage >= book->totalPageCount-2)
    {
        middleButtonAction();
        return;
    }
    book->currentPage += 2;


    renderer->drawBattery(rightPageFrameBufferNext,Device::getInstance().getBatteryPercentage());
    ESP_LOGI(TAG, "image present array: %d %d %d",
         static_cast<int>(pageImagePresent[0]),
         static_cast<int>(pageImagePresent[1]),
         static_cast<int>(pageImagePresent[2]));
    std::rotate(pageChapterIndex.begin(), pageChapterIndex.begin() + 1, pageChapterIndex.end());
    std::rotate(pageElementIndex.begin(), pageElementIndex.begin() + 1, pageElementIndex.end());
    std::rotate(pageImagePresent.begin(), pageImagePresent.begin() + 1, pageImagePresent.end());
    if((imagePreviouslyPresent!=pageImagePresent[1]) || pageImagePresent[1]) 
    {
        if(Device::getInstance().deviceSettings.smartImageDetect)
        {
            if( (100*abs(countBlackPixels(leftPageFrameBufferNext)-countBlackPixels(leftPageFrameBuffer)))/(648*480)>15 ||  
            (100*abs(countBlackPixels(rightPageFrameBufferNext)-countBlackPixels(rightPageFrameBuffer)))/(648*480)>15) //if the image is small and the overall page blackness level doesn't change too much, we don't need a full refresh
            {
                renderer->epd.forceRefresh();
            }
        }
        else   
        {
            renderer->epd.forceRefresh();
        }
    }
    imagePreviouslyPresent=pageImagePresent[1];

    this->renderer->epd.DisplayPictureBoth(
        leftPageFrameBufferNext,
        rightPageFrameBufferNext,
        [this]() {
            unsigned char* tempLeft = leftPageFrameBufferPrevious;
            leftPageFrameBufferPrevious = leftPageFrameBuffer;
            leftPageFrameBuffer = leftPageFrameBufferNext;
            leftPageFrameBufferNext = tempLeft;

            unsigned char* tempRight = rightPageFrameBufferPrevious;
            rightPageFrameBufferPrevious = rightPageFrameBuffer;
            rightPageFrameBuffer = rightPageFrameBufferNext;
            rightPageFrameBufferNext = tempRight;

            book->currentPage += 2;
            renderPages(leftPageFrameBufferNext, rightPageFrameBufferNext,2);
            book->currentPage -= 2;
        }
    );
    book->currentPageElementIndex = pageElementIndex[1]; //store what chapter and html element the current page is at
    book->currentPageChapterIndex = pageChapterIndex[1];
    ESP_LOGI(TAG, "image present array: %d %d %d",
         static_cast<int>(pageImagePresent[0]),
         static_cast<int>(pageImagePresent[1]),
         static_cast<int>(pageImagePresent[2]));

    if(Device::getInstance().buttonLatchedStates[MIDDLE_BUTTON]) {middleButtonAction(); return;}
    if(Device::getInstance().buttonLatchedStates[PAGE_RIGHT_BUTTON]) {rightPageAction(); return;}
    if(Device::getInstance().buttonLatchedStates[PAGE_LEFT_BUTTON]) {leftPageAction(); return;}
    if(Device::getInstance().buttonLatchedStates[ARROW_DOWN_BUTTON]) {downButtonAction(); return;}
    if(Device::getInstance().buttonLatchedStates[ARROW_UP_BUTTON]) {upButtonAction(); return;}
}

void Reader::prevPage()
{
    resetComicZoom(true);
    Device::getInstance().clearButtonLatches();
    Device::getInstance().setLatchTimeOut(100000);
    if(book->currentPage>=2)
    {
        book->currentPage -= 2;
        renderer->drawBattery(rightPageFrameBufferPrevious,Device::getInstance().getBatteryPercentage());
        ESP_LOGI(TAG, "image present array: %d %d %d",
         static_cast<int>(pageImagePresent[0]),
         static_cast<int>(pageImagePresent[1]),
         static_cast<int>(pageImagePresent[2]));
        std::rotate(pageChapterIndex.begin(), pageChapterIndex.begin() + 2, pageChapterIndex.end());
        std::rotate(pageElementIndex.begin(), pageElementIndex.begin() + 2, pageElementIndex.end());
        std::rotate(pageImagePresent.begin(), pageImagePresent.begin() + 2, pageImagePresent.end());
        
        if((imagePreviouslyPresent!=pageImagePresent[1]) || pageImagePresent[1]) 
        {
            if(Device::getInstance().deviceSettings.smartImageDetect)
            {
                if( (100*abs(countBlackPixels(leftPageFrameBufferPrevious)-countBlackPixels(leftPageFrameBuffer)))/(648*480)>15 ||  
                (100*abs(countBlackPixels(rightPageFrameBufferPrevious)-countBlackPixels(rightPageFrameBuffer)))/(648*480)>15) //if the image is small and the overall page blackness level doesn't change too much, we don't need a full refresh
                {
                    renderer->epd.forceRefresh();
                }    
            }
            else
            {
                renderer->epd.forceRefresh();
            } 
        }
        imagePreviouslyPresent=pageImagePresent[1];

        this->renderer->epd.DisplayPictureBoth(
            leftPageFrameBufferPrevious,
            rightPageFrameBufferPrevious,
            [this]() {
                unsigned char* tempLeft = leftPageFrameBufferNext;
                leftPageFrameBufferNext = leftPageFrameBuffer;
                leftPageFrameBuffer = leftPageFrameBufferPrevious;
                leftPageFrameBufferPrevious = tempLeft;

                unsigned char* tempRight = rightPageFrameBufferNext;
                rightPageFrameBufferNext = rightPageFrameBuffer;
                rightPageFrameBuffer = rightPageFrameBufferPrevious;
                rightPageFrameBufferPrevious = tempRight;

                if(book->currentPage>=2)
                {
                    book->currentPage -= 2;
                    renderPages(leftPageFrameBufferPrevious, rightPageFrameBufferPrevious,0);
                    book->currentPage += 2;
                }
            }
        );
        book->currentPageElementIndex = pageElementIndex[1]; //store what chapter and html element the current page is at
        book->currentPageChapterIndex = pageChapterIndex[1];
        ESP_LOGI(TAG, "image present array: %d %d %d",
         static_cast<int>(pageImagePresent[0]),
         static_cast<int>(pageImagePresent[1]),
         static_cast<int>(pageImagePresent[2]));
    }
    else
    {
        middleButtonAction();
    }

    if(Device::getInstance().buttonLatchedStates[MIDDLE_BUTTON]) {middleButtonAction(); return;}
    if(Device::getInstance().buttonLatchedStates[PAGE_RIGHT_BUTTON]) {rightPageAction(); return;}
    if(Device::getInstance().buttonLatchedStates[PAGE_LEFT_BUTTON]) {leftPageAction(); return;}
    if(Device::getInstance().buttonLatchedStates[ARROW_DOWN_BUTTON]) {downButtonAction(); return;}
    if(Device::getInstance().buttonLatchedStates[ARROW_UP_BUTTON]) {upButtonAction(); return;}
}

void Reader::indexPages(void)
{
    book->chapterPageCounts.clear();
    book->cachedImages.clear();
    book->totalPageCount = 0;
    book->chapterCount = this->epub->get_spine_items_count();
    for(int i=0;i<epub->get_spine_items_count();i++)
    {
        std::string currentChapterPath = epub->get_spine_item(i);
        char *html = reinterpret_cast<char *>(this->epub->get_item_contents(currentChapterPath));
        if(html)
        {
            HtmlParser *parser = nullptr;
            ESP_LOGI(TAG, "render Chapter: %s", currentChapterPath.c_str());
            parser = new HtmlParser(html, strlen(html), currentChapterPath,this->renderer,-2,this->epub,leftPageFrameBuffer,rightPageFrameBuffer);
            parser->indexingMode = true;
            parser->currentChapterIndex = i;
            parser->currentChapterStartPageIndex = book->totalPageCount;
            parser->bookMarks = book->bookMarks;
            parser->cachedImages = book->cachedImages;
            Book::BookMark currentPageData = Book::BookMark(book->currentPage,book->currentPageChapterIndex,book->currentPageElementIndex);
            parser->bookMarks.push_back(currentPageData);
            ESP_LOGI(TAG, "requested element: %d", book->currentPageElementIndex);
            parser->parse();
            if(book->currentPageChapterIndex==i) 
            {
                book->currentPage = parser->bookMarks[parser->bookMarks.size()-1].pageIndex;
                if(book->currentPage%2) book->currentPage-=1;
            }
            parser->bookMarks.pop_back();
            book->bookMarks = parser->bookMarks;
            book->cachedImages = parser->cachedImages;
            int pageCount = (parser->pageReturn+1);
            if(pageCount%2) pageCount+=1;
            book->chapterPageCounts.push_back(pageCount);
           book->totalPageCount += pageCount;
            free(html);
            delete parser;
        } 
    }
    clearChapterCache();
}

void Reader::addBookMark()
{
    for(int i=0;i<book->bookMarks.size();i++)
    {
        if(book->bookMarks[i].pageIndex==book->currentPage)
        {
            book->bookMarks.erase(book->bookMarks.begin() + i);
            return;
        }
    }

    book->bookMarks.push_back(Book::BookMark(book->currentPage,pageChapterIndex[1],pageElementIndex[1])); //store the data on the current bookmark
}

bool Reader::checkBookMark()
{
    for(int i=0;i<book->bookMarks.size();i++)
    {
        if(book->bookMarks[i].pageIndex==book->currentPage)
        {
            return true;
        }
    }
    return false;
}

int Reader::findCurrentBookMarkIndex()
{
    for(int i=0;i<book->bookMarks.size();i++)
    {
        if(book->bookMarks[i].pageIndex==book->currentPage)
        {
            return i;
        }
    }
    return -1;
}

void Reader::rightButtonAction() //the right and left buttons are no longer used
{
    // if(state==State::Reading)
    // {
    //     //Device::buzz();
    //     nextChapter();
    //     //Device::getInstance().bookHandler->saveBook(book);
    // }
    // //Device::buzz();
}

void Reader::leftButtonAction()
{
    // if(state==State::Reading)
    // {
    //     //Device::buzz();
    //     prevChapter();
    //     //Device::getInstance().bookHandler->saveBook(book);
    // }
    // //Device::buzz();
}

void Reader::upButtonAction()
{
    if(state==State::Reading)
    {
        if (hasOpenComic()) {
            Device::getInstance().clearButtonLatches();
            bool held = false;
            if (gpio_get_level(ARROW_UP_BUTTON) == 0) {
                held = true;
                int64_t startUs = esp_timer_get_time();
                while ((esp_timer_get_time() - startUs) < 300000) {
                    if (gpio_get_level(ARROW_UP_BUTTON) != 0) {
                        held = false;
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
            }
            if (!held) {
                zoomComicOut();
            } else {
                while (gpio_get_level(ARROW_UP_BUTTON) == 0) {
                    if (!panComic(0, -COMIC_PAN_STEP_Y)) {
                        while (gpio_get_level(ARROW_UP_BUTTON) == 0) {
                            vTaskDelay(pdMS_TO_TICKS(20));
                        }
                        break;
                    }
                }
            }
            Device::getInstance().pollButtons();
            Device::getInstance().clearButtonLatches();
            Device::getInstance().setLatchTimeOut(100000);
            return;
        }
        //Device::buzz();
        state = State::BookMarkMenu;
        bookMarkMenuHandler = new BookMarkMenuHandler(book,renderer,epub,leftPageFrameBuffer,rightPageFrameBuffer);
        bookMarkMenuHandler->bookMarkOnPage = checkBookMark();
        bookMarkMenuHandler->currentTocIndex = epub ? epub->get_toc_index_for_spine_index(getCurrentChapter(book->currentPage)) : 0;
        bookMarkMenuHandler->selectedBookMarkIndex = findCurrentBookMarkIndex();
        doWhileListening([&]() {bookMarkMenuHandler->drawMenu();});
    }
    else if(state==State::BookMarkMenu)
    {
        if(bookMarkMenuHandler->currentVerticalElementIndex<bookMarkMenuHandler->maxVerticalElements)
        {
            //Device::buzz();
            bookMarkMenuHandler->currentVerticalElementIndex+=1;
            doWhileListening([&]() {bookMarkMenuHandler->drawMenu();});
        }
    }
}

void Reader::downButtonAction()
{
    if(state==State::Reading)
    {
        if (hasOpenComic()) {
            Device::getInstance().clearButtonLatches();
            bool held = false;
            if (gpio_get_level(ARROW_DOWN_BUTTON) == 0) {
                held = true;
                int64_t startUs = esp_timer_get_time();
                while ((esp_timer_get_time() - startUs) < 300000) {
                    if (gpio_get_level(ARROW_DOWN_BUTTON) != 0) {
                        held = false;
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
            }
            if (!held || !isComicZoomed()) {
                zoomComicIn();
                if (held) {
                    while (gpio_get_level(ARROW_DOWN_BUTTON) == 0) {
                        vTaskDelay(pdMS_TO_TICKS(20));
                    }
                }
            } else {
                while (gpio_get_level(ARROW_DOWN_BUTTON) == 0) {
                    if (!panComic(0, COMIC_PAN_STEP_Y)) {
                        while (gpio_get_level(ARROW_DOWN_BUTTON) == 0) {
                            vTaskDelay(pdMS_TO_TICKS(20));
                        }
                        break;
                    }
                }
            }
            Device::getInstance().pollButtons();
            Device::getInstance().clearButtonLatches();
            Device::getInstance().setLatchTimeOut(100000);
            return;
        }
        //Device::buzz();
        addBookMark();
        if(checkBookMark()) renderer->drawBookMark(rightPageFrameBuffer,false);
        else renderer->drawBookMark(rightPageFrameBuffer,true);
        this->renderer->epd.DisplayPictureBoth(leftPageFrameBuffer,rightPageFrameBuffer);
        //Device::getInstance().bookHandler->saveBook(book);
    }
    else if(state==State::BookMarkMenu)
    {
        //Device::buzz();
        // if(bookMarkMenuHandler->selectedBookMarkIndex<  static_cast<int>(book->bookMarks.size())-1) bookMarkMenuHandler->selectedBookMarkIndex++;
        // bookMarkMenuHandler->drawMenu();
        if(bookMarkMenuHandler->currentVerticalElementIndex<=0)
        {
            state = State::Reading;
            delete bookMarkMenuHandler;
            openPage();
        }
        else
        {
            bookMarkMenuHandler->currentVerticalElementIndex-=1;
            doWhileListening([&]() {bookMarkMenuHandler->drawMenu();});
        }

    }

}

void Reader::leftPageAction()
{
    if(state==State::Reading)
    {
        if (isComicZoomed()) {
            Device::getInstance().clearButtonLatches();
            if (!panComic(-COMIC_PAN_STEP_X, 0)) {
                while (gpio_get_level(PAGE_LEFT_BUTTON) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
            }
            Device::getInstance().pollButtons();
            Device::getInstance().clearButtonLatches();
            Device::getInstance().setLatchTimeOut(100000);
            return;
        }
        //Device::buzz();
        prevPage();
        //Device::getInstance().bookHandler->saveBook(book);
    }
    else if(state==State::BookMarkMenu)
    {
        //Device::buzz();
        bookMarkMenuHandler->currentMenuElementIndex-=1;
        if(bookMarkMenuHandler->currentMenuElementIndex<0) bookMarkMenuHandler->currentMenuElementIndex=bookMarkMenuHandler->menuElements.size()-1;
        if(bookMarkMenuHandler->menuElements[bookMarkMenuHandler->currentMenuElementIndex].ID==BookMarkMenuHandler::MenuElementID::GotoChapter) bookMarkMenuHandler->currentVerticalElementIndex = bookMarkMenuHandler->currentTocIndex+1;
        else bookMarkMenuHandler->currentVerticalElementIndex = 0;
        doWhileListening([&]() {bookMarkMenuHandler->drawMenu();});
    }
}

void Reader::rightPageAction()
{
    if(state==State::Reading)
    {
        if (isComicZoomed()) {
            Device::getInstance().clearButtonLatches();
            if (!panComic(COMIC_PAN_STEP_X, 0)) {
                while (gpio_get_level(PAGE_RIGHT_BUTTON) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
            }
            Device::getInstance().pollButtons();
            Device::getInstance().clearButtonLatches();
            Device::getInstance().setLatchTimeOut(100000);
            return;
        }
        //Device::buzz();
        nextPage();
        //Device::getInstance().bookHandler->saveBook(book);
    }
    else if(state==State::BookMarkMenu)
    {
        //Device::buzz();
        bookMarkMenuHandler->currentMenuElementIndex+=1;
        if(bookMarkMenuHandler->currentMenuElementIndex>bookMarkMenuHandler->menuElements.size()-1) bookMarkMenuHandler->currentMenuElementIndex=0;
        if(bookMarkMenuHandler->menuElements[bookMarkMenuHandler->currentMenuElementIndex].ID==BookMarkMenuHandler::MenuElementID::GotoChapter) bookMarkMenuHandler->currentVerticalElementIndex = bookMarkMenuHandler->currentTocIndex+1;
        else bookMarkMenuHandler->currentVerticalElementIndex = 0;
        doWhileListening([&]() {bookMarkMenuHandler->drawMenu();});
    }
}


void Reader::middleButtonAction()
{
    if(state==State::Reading)
    {
       // Device::buzz();
        resetComicZoom(true);
        if (epub) {
            delete epub;
            epub = nullptr;
        }
        if (cbz) {
            delete cbz;
            cbz = nullptr;
        }
        if (cbr) {
            delete cbr;
            cbr = nullptr;
        }
        if (pdf) {
            delete pdf;
            pdf = nullptr;
        }
        Device::getInstance().state=Device::State::Menu;
        //Device::getInstance()->saveAppState();
        renderer->epd.forceRefresh();

        //std::static_pointer_cast<BookElement> (
        //std::static_pointer_cast<MenuElement>(Device::getInstance().menuHandler->currentElement)->children[
        //std::static_pointer_cast<MenuElement>(Device::getInstance().menuHandler->currentElement)->selectedChildIndex
        //])->elementDescription = std::to_string(book->currentPage+1) + "/" + std::to_string(book->totalPageCount); //update the pagecount of the book icon //actually book descriptions auto update now

        Device::getInstance().bookHandler->saveBook(book);
        Device::getInstance().menuHandler->drawMenu();
        Device::getInstance().saveAppState();
        //Device::buzz();
    }
    else if(state==State::BookMarkMenu)
    {
        // if(bookMarkMenuHandler->selectedBookMarkIndex!=-1) book->currentPage=book->bookMarks[bookMarkMenuHandler->selectedBookMarkIndex];
        // state = State::Reading;
        // delete bookMarkMenuHandler;
        // openPage();
        if(bookMarkMenuHandler->menuElements[bookMarkMenuHandler->currentMenuElementIndex].ID==BookMarkMenuHandler::MenuElementID::Return)
        {
            //Device::buzz();
                state = State::Reading;
            delete bookMarkMenuHandler;
            renderer->epd.forceRefresh();
            openPage();
            //Device::getInstance().bookHandler->saveBook(book);
        }
        else if(bookMarkMenuHandler->menuElements[bookMarkMenuHandler->currentMenuElementIndex].ID==BookMarkMenuHandler::MenuElementID::pageDisplay)
        {
            //Device::buzz();
            Device::getInstance().deviceSettings.showPagePercentage = !Device::getInstance().deviceSettings.showPagePercentage;
                state = State::Reading;
            delete bookMarkMenuHandler;
            renderer->epd.forceRefresh();
            Device::getInstance().saveSettings();
            openPage();
            //Device::getInstance().bookHandler->saveBook(book);
        }
        else if(bookMarkMenuHandler->menuElements[bookMarkMenuHandler->currentMenuElementIndex].ID==BookMarkMenuHandler::MenuElementID::NextChapt)
        {
            //Device::buzz();
            state = State::Reading;
            delete bookMarkMenuHandler;
            renderer->epd.forceRefresh();
            nextChapter();
            //Device::getInstance().bookHandler->saveBook(book);
        }
        else if(bookMarkMenuHandler->menuElements[bookMarkMenuHandler->currentMenuElementIndex].ID==BookMarkMenuHandler::MenuElementID::PrevChapt)
        {
            //Device::buzz();
            state = State::Reading;
            delete bookMarkMenuHandler;
            renderer->epd.forceRefresh();
            prevChapter();
            //Device::getInstance().bookHandler->saveBook(book);
        }
        else if(bookMarkMenuHandler->menuElements[bookMarkMenuHandler->currentMenuElementIndex].ID==BookMarkMenuHandler::MenuElementID::AddMark)
        {
            //Device::buzz();
            addBookMark();
            bookMarkMenuHandler->bookMarkOnPage = !bookMarkMenuHandler->bookMarkOnPage;
            if(checkBookMark()) renderer->drawBookMark(rightPageFrameBuffer,false);
            else renderer->drawBookMark(rightPageFrameBuffer,true);
            doWhileListening([&]() {bookMarkMenuHandler->drawMenu();});
            this->renderer->epd.DisplayPictureBoth(leftPageFrameBuffer,rightPageFrameBuffer);
            //Device::getInstance().bookHandler->saveBook(book);
        }
        else if(bookMarkMenuHandler->menuElements[bookMarkMenuHandler->currentMenuElementIndex].ID==BookMarkMenuHandler::MenuElementID::ToggleNightMode)
        {
            //Device::buzz();
            Device::getInstance().deviceSettings.nightMode = !Device::getInstance().deviceSettings.nightMode;
            //this->renderer->epd.DisplayPictureBoth(leftPageFrameBuffer,rightPageFrameBuffer);
            Device::getInstance().saveSettings();
            renderer->epd.forceRefresh();
            doWhileListening([&]() {bookMarkMenuHandler->drawMenu();});
        }
        else if(bookMarkMenuHandler->menuElements[bookMarkMenuHandler->currentMenuElementIndex].ID==BookMarkMenuHandler::MenuElementID::ToggleSunlightMode)
        {
            //Device::buzz();
            Device::getInstance().deviceSettings.sunlightMode = !Device::getInstance().deviceSettings.sunlightMode;
            //this->renderer->epd.DisplayPictureBoth(leftPageFrameBuffer,rightPageFrameBuffer);
            Device::getInstance().saveSettings();
            renderer->epd.forceRefresh();
            renderer->drawBattery(rightPageFrameBuffer,Device::getInstance().getBatteryPercentage());
            doWhileListening([&]() {bookMarkMenuHandler->drawMenu();});
        }
        else if(bookMarkMenuHandler->menuElements[bookMarkMenuHandler->currentMenuElementIndex].ID==BookMarkMenuHandler::MenuElementID::GotoMark)
        {
            if(bookMarkMenuHandler->currentVerticalElementIndex!=0)
            {
                //Device::buzz();
                book->currentPage=book->bookMarks[bookMarkMenuHandler->currentVerticalElementIndex-1].pageIndex;
                state = State::Reading;
                delete bookMarkMenuHandler;
                openPage();  
            }
        }
        else if(bookMarkMenuHandler->menuElements[bookMarkMenuHandler->currentMenuElementIndex].ID==BookMarkMenuHandler::MenuElementID::GotoChapter)
        {
            if(bookMarkMenuHandler->currentVerticalElementIndex!=0 && epub)
            {
                //Device::buzz();
                int newChapterIndex = epub->get_spine_index_for_toc_index(bookMarkMenuHandler->currentVerticalElementIndex-1);
                book->currentPage = getCurrentPageAbsolute(newChapterIndex,0);
                state = State::Reading;
                delete bookMarkMenuHandler;
                openPage();  
            }
        }
        
    }
}
