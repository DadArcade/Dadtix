#include "bookHandler.h"
#include "cbzHandler.h"
#include "cbrHandler.h"
#include "htmlParser.h"
#include "reader.h"
#include "cJSON.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include "device.h"
#include "esp_log.h"
static const char *TAG = "Bookhandler";

RTC_NOINIT_ATTR char rtc_currently_parsing_book[MAX_BOOK_NAME] = {0};
RTC_NOINIT_ATTR char rtc_currently_parsing_step[MAX_PARSE_STEP] = {0};

void rtc_setParsingStep(const char *step)
{
    if (!step) {
        rtc_currently_parsing_step[0] = '\0';
        return;
    }
    std::strncpy(rtc_currently_parsing_step, step, MAX_PARSE_STEP - 1);
    rtc_currently_parsing_step[MAX_PARSE_STEP - 1] = '\0';
}

bool ends_with_epub(const char *filename) {
    if (!filename || filename[0] == '.') {
        return false;   // reject hidden files
    }

    const char *ext = strrchr(filename, '.');
    return ext && strcasecmp(ext, ".epub") == 0;
}


bool rtc_isCurrentlyParsing(const std::string& bookPath)
{
    if (rtc_currently_parsing_book[0] == '\0') {
        return false;
    }
    return bookPath == std::string(rtc_currently_parsing_book);
}

void rtc_setCurrentlyParsing(const std::string& bookPath)
{
    std::strncpy(rtc_currently_parsing_book,
                 bookPath.c_str(),
                 MAX_BOOK_NAME - 1);
    rtc_currently_parsing_book[MAX_BOOK_NAME - 1] = '\0';
    rtc_setParsingStep("start");
}

void rtc_clearCurrentlyParsing()
{
    rtc_currently_parsing_book[0] = '\0';
    rtc_currently_parsing_step[0] = '\0';
}

std::string rtc_getCurrentlyParsing()
{
    return std::string(rtc_currently_parsing_book);
}

// simple suffix check
static bool ends_with(const char *str, const char *suffix) {
    if (!str || !suffix) return false;
    size_t l1 = strlen(str);
    size_t l2 = strlen(suffix);
    if (l2 > l1) return false;
    return strcmp(str + l1 - l2, suffix) == 0;
}

// ensure the /bookStorage/books directory exists
static void ensureBooksDir() {
    const char *dirpath = "/littlefs/books";
    struct stat st;
    if (stat(dirpath, &st) != 0) {
        // doesn't exist -> try to create (recursive mkdir not required here since /bookStorage should exist)
        if (mkdir(dirpath, 0777) != 0) {
            ESP_LOGE(TAG, "mkdir(%s) failed: %s", dirpath, strerror(errno));
        } else {
            ESP_LOGI(TAG, "Created directory: %s", dirpath);
        }
    } else {
        if (!S_ISDIR(st.st_mode)) {
            ESP_LOGW(TAG, "%s exists but is not a directory", dirpath);
        } // else directory exists and is fine
    }
}

Book::Book(std::string path, std::string title, std::string author, int totalPageCount)
{
    this->author = std::move(author);
    this->title = std::move(title);
    this->path = std::move(path);
    this->currentPage = 0;
    this->totalPageCount = totalPageCount;
    this->chapterCount = 0;
    this->chapterPageCounts.clear();
    // renderSettings default constructed
}

Book::Book(std::string path,
           std::string title,
           std::string author,
           int totalPageCount,
           int chapterCount,
           const std::vector<int>& chapterPageCounts,
           const RenderSettings& renderSettings)
{
    this->path = std::move(path);
    this->title = std::move(title);
    this->author = std::move(author);
    this->totalPageCount = totalPageCount;
    this->chapterCount = chapterCount;
    this->chapterPageCounts = chapterPageCounts;
    this->currentPage = 0;
    this->renderSettings = renderSettings;
}

Book::~Book()
{
    // nothing special; BookHandler will own/free Book pointers
}

Author::Author(std::string name)
{
    this->name = name;
}

void BookHandler::initFilesystem() {
    //mount the bookstorage filesystem. book metadata is stored here
    esp_vfs_littlefs_conf_t conf = {  
        .base_path = "/littlefs",
        .partition_label = "bookStorage",
        .format_if_mount_failed = true,
        .read_only = false,
    };
    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE("FS", "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE("FS", "Failed to find LittleFS partition");
        } else {
            ESP_LOGE("FS", "Failed to init LittleFS (%s)", esp_err_to_name(ret));
        }
    } else {
        size_t total, used;
        esp_littlefs_info("bookStorage", &total, &used);
        ESP_LOGI("FS", "LittleFS mounted. Partition size: total: %d, used: %d", total, used);
    }
    ensureBooksDir();

    //mount the assets filesystem. System epubs are stored here
    esp_vfs_littlefs_conf_t conf_2 = {
        .base_path = "/assets",
        .partition_label = "assets",
        .format_if_mount_failed = true,
        .read_only = false,
    };
    
    esp_err_t ret_2 = esp_vfs_littlefs_register(&conf_2);

    if (ret_2 != ESP_OK) {
        ESP_LOGE("FS", "assets mount failed: %s", esp_err_to_name(ret_2));
    } else {
        ESP_LOGI("FS", "assets mounted");
    }
}

std::string Book::toJSON() const {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "path", path.c_str());
    cJSON_AddStringToObject(root, "title", title.c_str());
    cJSON_AddStringToObject(root, "author", author.c_str());
    cJSON_AddNumberToObject(root, "totalPageCount", totalPageCount);
    cJSON_AddNumberToObject(root, "chapterCount", chapterCount);
    cJSON_AddNumberToObject(root, "currentPageChapterIndex", currentPageChapterIndex);
    cJSON_AddNumberToObject(root, "currentPageElementIndex", currentPageElementIndex);
    cJSON_AddNumberToObject(root, "currentPage", currentPage);
    cJSON_AddNumberToObject(root, "badParse", badParse);
    if (!parseError.empty()) {
        cJSON_AddStringToObject(root, "parseError", parseError.c_str());
    }
    if (!parseLog.empty()) {
        cJSON *logArr = cJSON_CreateArray();
        for (const std::string &line : parseLog) {
            cJSON_AddItemToArray(logArr, cJSON_CreateString(line.c_str()));
        }
        cJSON_AddItemToObject(root, "parseLog", logArr);
    }
    cJSON_AddNumberToObject(root, "favorite", favorite);
    cJSON_AddNumberToObject(root, "lastReadCounter", lastReadCounter);

    // chapters
    cJSON *chapters = cJSON_CreateArray();
    for (int v : chapterPageCounts) {
        cJSON_AddItemToArray(chapters, cJSON_CreateNumber(v));
    }
    cJSON_AddItemToObject(root, "chapterPageCounts", chapters);

    // bookmarks (as arrays [page, chapter, element])
    cJSON *bmArray = cJSON_CreateArray();
    for (const BookMark &bm : bookMarks) {
        cJSON *arr = cJSON_CreateArray();
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(bm.pageIndex));
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(bm.chapterIndex));
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(bm.elementIndex));
        cJSON_AddItemToArray(bmArray, arr);
    }
    cJSON_AddItemToObject(root, "bookMarks", bmArray);

    // render settings
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "fontSize", renderSettings.fontSize);
    cJSON_AddNumberToObject(r, "fontBold", renderSettings.fontBold);
    cJSON_AddNumberToObject(r, "marginsHorizontal", renderSettings.marginsHorizontal);
    cJSON_AddNumberToObject(r, "marginsVertical", renderSettings.marginsVertical);
    cJSON_AddNumberToObject(r, "fontPoints", renderSettings.fontPoints);
    cJSON_AddNumberToObject(r, "lineSpacing", renderSettings.lineSpacing);
    cJSON_AddStringToObject(r, "fontName", renderSettings.fontFamily.c_str());
    cJSON_AddItemToObject(root, "renderSettings", r);

    // cached images
    cJSON *imgArray = cJSON_CreateArray();
    for (const cachedImage &img : cachedImages) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "filePath", img.filePath.c_str());
        cJSON_AddNumberToObject(obj, "width", img.width);
        cJSON_AddNumberToObject(obj, "height", img.height);
        cJSON_AddItemToArray(imgArray, obj);
    }
    cJSON_AddItemToObject(root, "cachedImages", imgArray);

    // finalize
    char *jsonStr = cJSON_PrintUnformatted(root);
    std::string result = jsonStr ? std::string(jsonStr) : "{}";
    if (jsonStr) cJSON_free(jsonStr);
    cJSON_Delete(root);
    return result;
}


Book *Book::fromJSON(const std::string &json) {
    cJSON *root = cJSON_Parse(json.c_str());
    if (!root) return nullptr;

    cJSON *jpath = cJSON_GetObjectItem(root, "path");
    cJSON *jtitle = cJSON_GetObjectItem(root, "title");
    cJSON *jauthor = cJSON_GetObjectItem(root, "author");
    cJSON *jpages = cJSON_GetObjectItem(root, "totalPageCount");
    

    if (!cJSON_IsString(jpath) || !cJSON_IsString(jtitle) || !cJSON_IsString(jauthor) || !cJSON_IsNumber(jpages)) {
        cJSON_Delete(root);
        return nullptr;
    }

    Book *book = new Book(jpath->valuestring, jtitle->valuestring, jauthor->valuestring, jpages->valueint);

    cJSON *jchapterCount = cJSON_GetObjectItem(root, "chapterCount");
    cJSON *jchapterPages = cJSON_GetObjectItem(root, "chapterPageCounts");
    cJSON *jread = cJSON_GetObjectItem(root, "currentPage");
    cJSON *jreadChapter = cJSON_GetObjectItem(root, "currentPageChapterIndex");
    cJSON *jreadElement = cJSON_GetObjectItem(root, "currentPageElementIndex");
    cJSON *jrender = cJSON_GetObjectItem(root, "renderSettings");
    cJSON *jbadParse = cJSON_GetObjectItem(root, "badParse");
    cJSON *jparseError = cJSON_GetObjectItem(root, "parseError");
    cJSON *jparseLog = cJSON_GetObjectItem(root, "parseLog");
    cJSON *jfavorite = cJSON_GetObjectItem(root, "favorite");
    cJSON *jlastRead = cJSON_GetObjectItem(root, "lastReadCounter");
    cJSON *jbookMarks = cJSON_GetObjectItem(root, "bookMarks");
    cJSON *jcachedImages = cJSON_GetObjectItem(root, "cachedImages");

    book->currentPageChapterIndex = cJSON_IsNumber(jreadChapter) ? jreadChapter->valueint : 0;
    book->currentPageElementIndex = cJSON_IsNumber(jreadElement) ? jreadElement->valueint : 0;
    book->chapterCount = cJSON_IsNumber(jchapterCount) ? jchapterCount->valueint : 0;
    book->currentPage = cJSON_IsNumber(jread) ? jread->valueint : 0;
    book->badParse = cJSON_IsNumber(jbadParse) ? jbadParse->valueint : false;
    if (cJSON_IsString(jparseError) && jparseError->valuestring) {
        book->parseError = jparseError->valuestring;
    }
    if (jparseLog && cJSON_IsArray(jparseLog)) {
        int logLen = cJSON_GetArraySize(jparseLog);
        for (int i = 0; i < logLen; ++i) {
            cJSON *item = cJSON_GetArrayItem(jparseLog, i);
            if (cJSON_IsString(item) && item->valuestring) {
                book->parseLog.push_back(item->valuestring);
            }
        }
    }
    book->favorite = cJSON_IsNumber(jfavorite) ? jfavorite->valueint : false;
    book->lastReadCounter = cJSON_IsNumber(jlastRead) ? (uint32_t)jlastRead->valueint : 0;

    // chapterPageCounts
    if (jchapterPages && cJSON_IsArray(jchapterPages)) {
        int chapterLen = cJSON_GetArraySize(jchapterPages);
        for (int i = 0; i < chapterLen; ++i) {
            cJSON *item = cJSON_GetArrayItem(jchapterPages, i);
            book->chapterPageCounts.push_back(cJSON_IsNumber(item) ? item->valueint : 0);
        }
    }

    // bookmarks (supports both new array and old int formats)
    if (jbookMarks && cJSON_IsArray(jbookMarks)) {
        int bmLen = cJSON_GetArraySize(jbookMarks);
        for (int i = 0; i < bmLen; ++i) {
            cJSON *item = cJSON_GetArrayItem(jbookMarks, i);
            BookMark bm{};
            if (cJSON_IsArray(item)) {
                // new format [page, chapter, element]
                bm.pageIndex = cJSON_IsNumber(cJSON_GetArrayItem(item, 0)) ? cJSON_GetArrayItem(item, 0)->valueint : 0;
                bm.chapterIndex = cJSON_IsNumber(cJSON_GetArrayItem(item, 1)) ? cJSON_GetArrayItem(item, 1)->valueint : 0;
                bm.elementIndex = cJSON_IsNumber(cJSON_GetArrayItem(item, 2)) ? cJSON_GetArrayItem(item, 2)->valueint : 0;
            } else if (cJSON_IsNumber(item)) {
                // legacy integer format
                bm.pageIndex = item->valueint;
                bm.chapterIndex = 0;
                bm.elementIndex = 0;
            }
            book->bookMarks.push_back(bm);
        }
    }

    // cachedImages
    if (jcachedImages && cJSON_IsArray(jcachedImages)) {
        int imgLen = cJSON_GetArraySize(jcachedImages);
        for (int i = 0; i < imgLen; ++i) {
            cJSON *item = cJSON_GetArrayItem(jcachedImages, i);
            if (!cJSON_IsObject(item)) continue;

            cachedImage img{};

            cJSON *jp = cJSON_GetObjectItem(item, "filePath");
            cJSON *jw = cJSON_GetObjectItem(item, "width");
            cJSON *jh = cJSON_GetObjectItem(item, "height");

            if (cJSON_IsString(jp)) img.filePath = jp->valuestring;
            if (cJSON_IsNumber(jw)) img.width = jw->valueint;
            if (cJSON_IsNumber(jh)) img.height = jh->valueint;

            book->cachedImages.push_back(img);
        }
    }

    // renderSettings
    if (jrender && cJSON_IsObject(jrender)) {
        cJSON *jf = cJSON_GetObjectItem(jrender, "fontSize");
        cJSON *jb = cJSON_GetObjectItem(jrender, "fontBold");
        cJSON *mh = cJSON_GetObjectItem(jrender, "marginsHorizontal");
        cJSON *mv = cJSON_GetObjectItem(jrender, "marginsVertical");
        cJSON *ls = cJSON_GetObjectItem(jrender, "lineSpacing");
        cJSON *fp = cJSON_GetObjectItem(jrender, "fontPoints");
        cJSON *fn = cJSON_GetObjectItem(jrender, "fontName");
        if (cJSON_IsNumber(jf)) book->renderSettings.fontSize = jf->valueint;
        if (cJSON_IsNumber(jb)) book->renderSettings.fontBold = jb->valueint;
        if (cJSON_IsNumber(mh)) book->renderSettings.marginsHorizontal = mh->valueint;
        if (cJSON_IsNumber(mv)) book->renderSettings.marginsVertical = mv->valueint;
        if (cJSON_IsNumber(ls)) book->renderSettings.lineSpacing = ls->valueint;
        if (cJSON_IsNumber(fp)) book->renderSettings.fontPoints = fp->valueint;
        if (cJSON_IsString(fn)) book->renderSettings.fontFamily = fn->valuestring;
    }

    cJSON_Delete(root);
    return book;
}



bool Book::isCbz() const {
    return isCbzFilename(this->path);
}

bool Book::isCbr() const {
    return isCbrFilename(this->path);
}

bool Book::isComic() const {
    return isComicFilename(this->path);
}

bool Book::matchesRenderSettings(const RenderSettings &current) const {
    if (isComic() && totalPageCount > 0) {
        return true;
    }
    return renderSettings.fontSize == current.fontSize &&
           renderSettings.fontBold == current.fontBold &&
           renderSettings.marginsHorizontal == current.marginsHorizontal &&
           renderSettings.marginsVertical == current.marginsVertical &&
           renderSettings.lineSpacing == current.lineSpacing &&
           renderSettings.fontPoints == current.fontPoints &&
           renderSettings.fontFamily == current.fontFamily;
}

Book::RenderSettings Book::getCurrentRenderSettings() {
    Device &dev = Device::getInstance();
    RenderSettings currentRS {
            dev.renderSettings.fontSize,
            dev.renderSettings.fontBold,
            dev.renderSettings.marginsHorizontal,
            dev.renderSettings.marginsVertical,
            dev.renderer->fontHandler.families[dev.renderSettings.fontFamily].name,
            dev.renderSettings.fontPoints,
            dev.renderSettings.lineSpacing,
        };
    return currentRS;
}

BookHandler::BookHandler()
{
    initFilesystem();
}

BookHandler::~BookHandler()
{
    // free all owned Book pointers
    for (auto &kv : indexedBooks) {
        delete kv.second;
    }
    indexedBooks.clear();
    // bookList contains pointers owned above, so clear it (no double delete)
    bookList.clear();
    authorList.clear();
}



void BookHandler::loadBooks()
{
    // Free existing books
    for (auto &kv : indexedBooks) {
        delete kv.second;
    }
    indexedBooks.clear();

    bool loadFromSD = Device::getInstance().deviceSettings.storeDataOnSD;
    const char *basePath = loadFromSD ? "/sdcard/book_data" : "/littlefs/books";

    DIR *dir = opendir(basePath);
    if (!dir) {
        ESP_LOGW(TAG, "No book storage directory found: %s", basePath);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type == DT_DIR) continue;

        std::string filename(entry->d_name);
        if (!ends_with(filename.c_str(), ".json")) continue;

        std::string fullPath = std::string(basePath) + "/" + filename;

        FILE *f = fopen(fullPath.c_str(), "r");
        if (!f) {
            ESP_LOGW(TAG, "Failed to open %s", fullPath.c_str());
            continue;
        }

        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);

        if (size <= 0) {
            fclose(f);
            continue;
        }

        std::vector<char> buffer(size + 1);
        fread(buffer.data(), 1, size, f);
        fclose(f);
        buffer[size] = '\0';

        Book *book = Book::fromJSON(buffer.data());
        if (book) {
            indexedBooks[book->path] = book;
        } else {
            ESP_LOGW(TAG, "Invalid book JSON: %s", fullPath.c_str());
        }
    }

    closedir(dir);

    ESP_LOGI(
        TAG,
        "Loaded %d books from %s",
        indexedBooks.size(),
        loadFromSD ? "SD card" : "flash"
    );
}


static std::string trimWhitespace(const std::string &str) {
    size_t start = str.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = str.find_last_not_of(" \t\r\n");
    return str.substr(start, end - start + 1);
}

static std::string stripAnnotations(const std::string &str) {
    std::string out;
    out.reserve(str.size());
    int parenDepth = 0;
    int bracketDepth = 0;
    for (char c : str) {
        if (c == '(') parenDepth++;
        else if (c == ')' && parenDepth > 0) parenDepth--;
        else if (c == '[') bracketDepth++;
        else if (c == ']' && bracketDepth > 0) bracketDepth--;
        else if (parenDepth == 0 && bracketDepth == 0) out += c;
    }
    return out;
}

static std::string normalizeForSort(const std::string &str) {
    std::string out;
    out.reserve(str.size());
    for (size_t i = 0; i < str.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(str[i]);
        if (c >= 'A' && c <= 'Z') {
            out += static_cast<char>(c + ('a' - 'A'));
        } else if (c == 0xD0 && i + 1 < str.size()) {
            unsigned char c2 = static_cast<unsigned char>(str[i + 1]);
            if (c2 == 0x81) {
                // Cyrillic 'Ё' (U+0401) -> 'е' (U+0435 = 0xD0 0xB5)
                out += static_cast<char>(0xD0);
                out += static_cast<char>(0xB5);
                ++i;
            } else if (c2 >= 0x90 && c2 <= 0x9F) {
                // Cyrillic 'А'..'П' (U+0410..U+041F) -> 'а'..'п' (0xD0 0xB0..0xBF)
                out += static_cast<char>(0xD0);
                out += static_cast<char>(c2 + 0x20);
                ++i;
            } else if (c2 >= 0xA0 && c2 <= 0xAF) {
                // Cyrillic 'Р'..'Я' (U+0420..U+042F) -> 'р'..'я' (0xD1 0x80..0x8F)
                out += static_cast<char>(0xD1);
                out += static_cast<char>(c2 - 0x20);
                ++i;
            } else {
                out += static_cast<char>(c);
            }
        } else if (c == 0xD1 && i + 1 < str.size() && static_cast<unsigned char>(str[i + 1]) == 0x91) {
            // Cyrillic 'ё' (U+0451) -> 'е' (U+0435 = 0xD0 0xB5)
            out += static_cast<char>(0xD0);
            out += static_cast<char>(0xB5);
            ++i;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

static bool isGenerationalSuffix(const std::string &token) {
    std::string clean;
    for (char c : token) {
        if (c != '.') clean += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return clean == "jr" || clean == "sr" || clean == "ii" || clean == "iii" || clean == "iv" || clean == "v";
}

static std::string getAuthorLastNameSortKey(const std::string &author) {
    std::string cleaned = trimWhitespace(stripAnnotations(author));
    std::string lowerFull = normalizeForSort(cleaned);
    if (lowerFull.empty() || lowerFull == "unknown author" || lowerFull == "error during parsing") {
        return "\xff";
    }

    // Extract primary author if multiple authors are listed
    size_t semiPos = cleaned.find(';');
    if (semiPos != std::string::npos) {
        cleaned = trimWhitespace(cleaned.substr(0, semiPos));
    }
    size_t ampPos = cleaned.find(" & ");
    if (ampPos != std::string::npos) {
        cleaned = trimWhitespace(cleaned.substr(0, ampPos));
    }
    std::string lowerCheck = normalizeForSort(cleaned);
    size_t andPos = lowerCheck.find(" and ");
    if (andPos != std::string::npos) {
        cleaned = trimWhitespace(cleaned.substr(0, andPos));
    }

    // Handle "Last, First" or "First Last, Jr."
    bool hasComma = false;
    size_t commaPos = cleaned.find(',');
    if (commaPos != std::string::npos) {
        std::string beforeComma = trimWhitespace(cleaned.substr(0, commaPos));
        if (!beforeComma.empty()) {
            cleaned = beforeComma;
            hasComma = true;
        }
    }

    // Split into whitespace-separated tokens
    std::vector<std::string> tokens;
    size_t pos = 0;
    while (pos < cleaned.size()) {
        size_t ws = cleaned.find_first_of(" \t\r\n", pos);
        if (ws == std::string::npos) {
            tokens.push_back(cleaned.substr(pos));
            break;
        }
        if (ws > pos) {
            tokens.push_back(cleaned.substr(pos, ws - pos));
        }
        pos = ws + 1;
    }

    if (tokens.empty()) return "\xff";

    if (tokens.size() > 1 && isGenerationalSuffix(tokens.back())) {
        tokens.pop_back();
    }

    std::string lastName = tokens.back();
    if (tokens.size() >= 2 && (tokens.size() >= 3 || hasComma)) {
        std::string prev = normalizeForSort(tokens[tokens.size() - 2]);
        if (prev == "le" || prev == "la" || prev == "van" || prev == "st" || prev == "st.") {
            lastName = tokens[tokens.size() - 2] + " " + lastName;
        }
    }

    return normalizeForSort(lastName);
}

static void sortBooksByAuthorLastName(std::vector<Book*> &books) {
    std::sort(books.begin(), books.end(), [](const Book *a, const Book *b) {
        if (!a || !b) return a != nullptr;

        std::string lastA = getAuthorLastNameSortKey(a->author);
        std::string lastB = getAuthorLastNameSortKey(b->author);
        if (lastA != lastB) return lastA < lastB;

        std::string fullAuthorA = normalizeForSort(a->author);
        std::string fullAuthorB = normalizeForSort(b->author);
        if (fullAuthorA != fullAuthorB) return fullAuthorA < fullAuthorB;
        if (a->author != b->author) return a->author < b->author;

        std::string titleA = normalizeForSort(a->title);
        std::string titleB = normalizeForSort(b->title);
        if (titleA != titleB) return titleA < titleB;

        return a->path < b->path;
    });
}

static std::string getBookMetaFilename(const std::string &bookPath) {
    std::string sanitized = bookPath;
    std::replace(sanitized.begin(), sanitized.end(), '/', '_');

    // LittleFS CONFIG_LITTLEFS_OBJ_NAME_LEN is 64 (max 63 chars + null terminator).
    // If "<sanitized>.json" exceeds 63 chars, truncate and append an 8-char FNV-1a hash.
    if (sanitized.size() + 5 > 63) {
        uint32_t hash = 2166136261u;
        for (unsigned char c : bookPath) {
            hash ^= c;
            hash *= 16777619u;
        }
        char hashBuf[10];
        snprintf(hashBuf, sizeof(hashBuf), "_%08x", (unsigned int)hash);
        sanitized = sanitized.substr(0, 63 - 5 - 9) + hashBuf;
    }

    return sanitized + ".json";
}

// save this->indexedBooks to NVS
void BookHandler::saveBook(Book *book)
{
    if (!book) return;

    const char *basePath = Device::getInstance().deviceSettings.storeDataOnSD ? "/sdcard/book_data" : "/littlefs/books";

    // Ensure directory exists
    struct stat st;
    if (stat(basePath, &st) != 0) {
        if (mkdir(basePath, 0777) != 0) {
            ESP_LOGE(TAG, "Failed to create directory: %s", basePath);
            return;
        }
    }

    std::string filename = std::string(basePath) + "/" + getBookMetaFilename(book->path);
    std::string jsonStr = book->toJSON();

    FILE *f = fopen(filename.c_str(), "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for writing", filename.c_str());
        return;
    }

    size_t written = fwrite(jsonStr.c_str(), 1, jsonStr.size(), f);
    fclose(f);

    if (written != jsonStr.size()) {
        ESP_LOGE(TAG, "Short write when saving book metadata: %s", filename.c_str());
        return;
    }

    ESP_LOGI(
        TAG,
        "Saved book metadata (%s): %s",
        Device::getInstance().deviceSettings.storeDataOnSD ? "SD" : "FLASH",
        filename.c_str()
    );
}


void BookHandler::deleteBook(const std::string &bookPath)
{
    Device::getInstance().removeRecentBook(bookPath);
    bool deleteFromSD = Device::getInstance().deviceSettings.storeDataOnSD;
    const char *basePath = deleteFromSD ? "/sdcard/book_data" : "/littlefs/books";

    std::string filename = std::string(basePath) + "/" + getBookMetaFilename(bookPath);

    if (remove(filename.c_str()) != 0) {
        ESP_LOGW(TAG, "Failed to delete book file: %s", filename.c_str());
    } else {
        ESP_LOGI(TAG, "Deleted book file: %s", filename.c_str());
    }

    auto it = indexedBooks.find(bookPath);
    if (it != indexedBooks.end()) {
        delete it->second;
        indexedBooks.erase(it);
    }
}

void BookHandler::transferDataToSD()
{
    const char *flashPath = "/littlefs/books";
    const char *sdPath    = "/sdcard/book_data";

    // Ensure SD directory exists
    struct stat st;
    if (stat(sdPath, &st) != 0) {
        if (mkdir(sdPath, 0777) != 0) {
            ESP_LOGE(TAG, "Failed to create SD book_data directory");
            return;
        }
    }

    DIR *dir = opendir(flashPath);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open flash books directory");
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type == DT_DIR) continue;

        std::string filename(entry->d_name);
        if (!ends_with(filename.c_str(), ".json")) continue;

        std::string srcPath  = std::string(flashPath) + "/" + filename;
        std::string destPath = std::string(sdPath)    + "/" + filename;

        FILE *src = fopen(srcPath.c_str(), "r");
        if (!src) {
            ESP_LOGW(TAG, "Failed to open source file: %s", srcPath.c_str());
            continue;
        }

        FILE *dst = fopen(destPath.c_str(), "w");
        if (!dst) {
            ESP_LOGW(TAG, "Failed to open destination file: %s", destPath.c_str());
            fclose(src);
            continue;
        }

        char buffer[512];
        size_t bytes;
        while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
            fwrite(buffer, 1, bytes, dst);
        }

        fclose(src);
        fclose(dst);

        ESP_LOGI(TAG, "Transferred book metadata: %s", filename.c_str());
    }

    closedir(dir);

    ESP_LOGI(TAG, "Book data transfer from flash to SD completed");
}

void BookHandler::transferDataToFlash()
{
    const char *sdPath    = "/sdcard/book_data";
    const char *flashPath = "/littlefs/books";

    // Ensure flash directory exists
    struct stat st;
    if (stat(flashPath, &st) != 0) {
        if (mkdir(flashPath, 0777) != 0) {
            ESP_LOGE(TAG, "Failed to create flash books directory");
            return;
        }
    }

    DIR *dir = opendir(sdPath);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open SD book_data directory");
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type == DT_DIR) continue;

        std::string filename(entry->d_name);
        if (!ends_with(filename.c_str(), ".json")) continue;

        std::string srcPath  = std::string(sdPath)    + "/" + filename;
        std::string destPath = std::string(flashPath) + "/" + filename;

        FILE *src = fopen(srcPath.c_str(), "r");
        if (!src) {
            ESP_LOGW(TAG, "Failed to open source file: %s", srcPath.c_str());
            continue;
        }

        FILE *dst = fopen(destPath.c_str(), "w");
        if (!dst) {
            ESP_LOGW(TAG, "Failed to open destination file: %s", destPath.c_str());
            fclose(src);
            continue;
        }

        char buffer[512];
        size_t bytes;
        while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
            fwrite(buffer, 1, bytes, dst);
        }

        fclose(src);
        fclose(dst);

        ESP_LOGI(TAG, "Transferred book metadata: %s", filename.c_str());
    }

    closedir(dir);

    ESP_LOGI(TAG, "Book data transfer from SD to flash completed");
}


static void captureBookFileDiagnostics(const std::string &fullPath, std::vector<std::string> &outLog)
{
    outLog.push_back("full_path=" + fullPath);

    struct stat st{};
    errno = 0;
    if (stat(fullPath.c_str(), &st) != 0) {
        int err = errno;
        outLog.push_back("stat=failed errno=" + std::to_string(err) + " (" + std::string(strerror(err)) + ")");
    } else {
        outLog.push_back("stat=ok size_bytes=" + std::to_string(static_cast<long long>(st.st_size)));
    }

    FILE *f = fopen(fullPath.c_str(), "rb");
    if (!f) {
        int err = errno;
        outLog.push_back("fopen_rb=failed errno=" + std::to_string(err) + " (" + std::string(strerror(err)) + ")");
    } else {
        unsigned char hdr[8] = {0};
        size_t n = fread(hdr, 1, sizeof(hdr), f);
        fclose(f);
        char hex[64] = {0};
        int pos = 0;
        for (size_t i = 0; i < n && pos < 56; ++i) {
            pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", hdr[i]);
        }
        const char *sigType = "unknown";
        if (n >= 7 && hdr[0] == 0x52 && hdr[1] == 0x61 && hdr[2] == 0x72 &&
            hdr[3] == 0x21 && hdr[4] == 0x1A && hdr[5] == 0x07) {
            sigType = (hdr[6] == 0x00) ? "RAR3/4" : ((hdr[6] == 0x01) ? "RAR5" : "RAR_future");
        } else if (n >= 4 && hdr[0] == 0x50 && hdr[1] == 0x4B) {
            sigType = "ZIP/CBZ/EPUB";
        }
        outLog.push_back(std::string("magic_8b=") + hex + "(" + sigType + ")");
    }

#ifdef ESP_PLATFORM
    char heapBuf[160];
    snprintf(heapBuf, sizeof(heapBuf),
             "heap free_psram=%zu largest_psram=%zu free_int=%zu largest_int=%zu",
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    outLog.push_back(heapBuf);
#endif
}

static Book *indexNewComicBook(
    const std::string &fileName,
    const std::string &fullPath,
    const Book::RenderSettings &currentRS,
    Device &dev)
{
    ESP_LOGI(TAG, "Indexing new comic archive: %s", fullPath.c_str());
    vTaskDelay(20);

    std::vector<std::string> diagLog;
    captureBookFileDiagnostics(fullPath, diagLog);
    std::string failReason = "Unknown comic archive parsing error";

    if (isCbzFilename(fileName)) {
        rtc_setParsingStep("cbz:open");
        CbzArchive cbz(fullPath);
        diagLog.push_back("cbz_open=" + std::string(cbz.isOpen() ? "true" : "false") +
                          " pages=" + std::to_string(cbz.getPageCount()));
        if (cbz.isOpen() && cbz.getPageCount() > 0) {
            std::string title = cbz.getTitle();
            std::string author = cbz.getAuthor();
            dev.notificationHandler->drawIndexingNotification(title);
            return new Book(fileName, title, author, cbz.getPageCount(), 0, std::vector<int>{}, currentRS);
        }
        failReason = cbz.isOpen()
            ? "CBZ opened successfully, but contains 0 supported image pages (.jpg/.jpeg/.png)"
            : "Failed to open CBZ archive via miniz";
    } else if (isCbrFilename(fileName)) {
        rtc_setParsingStep("cbr:init");
        CbrArchive cbr(fullPath);
        for (const std::string &step : cbr.getParseLog()) {
            diagLog.push_back(step);
        }
        if (cbr.isOpen() && cbr.getPageCount() > 0) {
            std::string title = cbr.getTitle();
            std::string author = cbr.getAuthor();
            dev.notificationHandler->drawIndexingNotification(title);
            return new Book(fileName, title, author, cbr.getPageCount(), 0, std::vector<int>{}, currentRS);
        }
        if (!cbr.getLastError().empty()) {
            failReason = cbr.getLastError();
        } else {
            failReason = "CBR archive failed to open or contains 0 supported image pages";
        }
    }

    dev.notificationHandler->drawErrorNotification(fileName);
    Book *book = new Book(fileName, fileName, std::string("error during parsing"), 0, 0, std::vector<int>{}, currentRS);
    book->badParse = true;
    book->parseError = failReason;
    book->parseLog = std::move(diagLog);
    vTaskDelay(100);
    return book;
}

void BookHandler::listBooks(void)
{
    if(authorList.empty()) authorList.emplace_back("Favorite books");
    const char *sdPath = "/sdcard";

    DIR *rootDir = opendir(sdPath);
    if (!rootDir) {
        ESP_LOGE(TAG, "Failed to open directory: %s", sdPath);
        return;
    }
    closedir(rootDir);

    Device &dev = Device::getInstance();
    if (dev.activeBookPath.empty()) dev.activeBookIndex = 0;

    // Step 1: Load all cached books from LittleFS
    loadBooks();  // fills indexedBooks with any JSON files found

    // Build current render settings snapshot
    Book::RenderSettings currentRS {
        dev.renderSettings.fontSize,
        dev.renderSettings.fontBold,
        dev.renderSettings.marginsHorizontal,
        dev.renderSettings.marginsVertical
    };

    // Step 2: Recursively scan /sdcard (and subfolders) and process each EPUB / CBZ.
    // Close each DIR* before opening the next so we never exceed FATFS max_files.
    std::vector<std::string> foundPaths;
    std::vector<std::string> dirsToVisit = {""};

    while (!dirsToVisit.empty()) {
        std::string relDir = std::move(dirsToVisit.back());
        dirsToVisit.pop_back();

        std::string fullDirPath = relDir.empty()
            ? std::string(sdPath)
            : (std::string(sdPath) + "/" + relDir);

        DIR *dir = opendir(fullDirPath.c_str());
        if (!dir) {
            ESP_LOGW(TAG, "Failed to open directory: %s", fullDirPath.c_str());
            continue;
        }

        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_name[0] == '.') continue; // skip ., .., and hidden files/folders

            if (entry->d_type == DT_DIR) {
                if (relDir.empty() &&
                    (strcmp(entry->d_name, "book_data") == 0 ||
                     strcmp(entry->d_name, "fonts") == 0 ||
                     strcasecmp(entry->d_name, "System Volume Information") == 0)) {
                    continue;
                }
                std::string subRelDir = relDir.empty()
                    ? std::string(entry->d_name)
                    : (relDir + "/" + entry->d_name);
                dirsToVisit.push_back(std::move(subRelDir));
            } else if (ends_with_epub(entry->d_name) || isComicFilename(entry->d_name)) {
                std::string relFilePath = relDir.empty()
                    ? std::string(entry->d_name)
                    : (relDir + "/" + entry->d_name);
                foundPaths.push_back(std::move(relFilePath));
            }
        }
        closedir(dir);
    }

    for (const std::string &fileName : foundPaths) {
        Book *book = nullptr;
        auto it = indexedBooks.find(fileName);

        if (it != indexedBooks.end()) {
            // Cached book exists: if a comic previously failed parsing, re-attempt automatically
            book = it->second;
            if (book->isComic() && (book->badParse || book->totalPageCount <= 0)) {
                std::string fullPath = std::string(sdPath) + "/" + fileName;
                rtc_setCurrentlyParsing(fileName);
                Book *reindexed = indexNewComicBook(fileName, fullPath, currentRS, dev);
                rtc_clearCurrentlyParsing();
                if (reindexed) {
                    if (!reindexed->badParse && reindexed->totalPageCount > 0) {
                        book->title = reindexed->title;
                        book->author = reindexed->author;
                        book->totalPageCount = reindexed->totalPageCount;
                        book->badParse = false;
                        book->parseError.clear();
                        book->parseLog.clear();
                    } else {
                        book->parseError = reindexed->parseError;
                        book->parseLog = reindexed->parseLog;
                    }
                    saveBook(book);
                    delete reindexed;
                }
            }
            this->bookList.push_back(book);  
        } else {
            // New book -> index and save
            std::string fullPath = std::string(sdPath) + "/" + fileName;
            if (rtc_isCurrentlyParsing(fileName)) { //if the book is malformed and somehow crashes the loader, this is triggered
                //notify the user about the error, and store the book as malformed with crash breadcrumbs
                std::string lastStep(rtc_currently_parsing_step);
                dev.notificationHandler->drawErrorNotification(fileName);
                book = new Book(fileName,
                                    fileName,
                                    std::string("error during parsing"),
                                    0,
                                    0,
                                    std::vector<int> {},
                                    currentRS);
                book->badParse = true;
                book->parseError = "Crash or watchdog reset while parsing at step: '" +
                                   (lastStep.empty() ? std::string("unknown") : lastStep) + "'";
                captureBookFileDiagnostics(fullPath, book->parseLog);
                book->parseLog.push_back("rtc_crash_recovery=1 last_step=" + lastStep);
                this->bookList.push_back(book);
                indexedBooks[fileName] = book;
                // Save immediately to LittleFS
                saveBook(book);
                rtc_clearCurrentlyParsing();
                vTaskDelay(100);
            }
            else if (isComicFilename(fileName))
            {
                rtc_setCurrentlyParsing(fileName);
                book = indexNewComicBook(fileName, fullPath, currentRS, dev);
                this->bookList.push_back(book);
                indexedBooks[fileName] = book;
                saveBook(book);
                rtc_clearCurrentlyParsing();
            }
            else
            {
                rtc_setCurrentlyParsing(fileName);
                Epub *epub = new Epub(fullPath);
                ESP_LOGI(TAG, "Indexing new book: %s", fullPath.c_str());
                vTaskDelay(20);

                if (epub->load()) {
                    std::string title = epub->get_title();
                    std::string author = epub->get_author();
                    dev.notificationHandler->drawIndexingNotification(title);

                    Reader reader;
                    book = new Book(fileName,
                                    title,
                                    author,
                                    0,
                                    0,
                                    std::vector<int> {},
                                    currentRS);
                    book->renderSettings.fontSize=-1; //change this to some bad value so it is forced to re-index on opening
                    //reader.init(book, nullptr);
                    //reader.indexPages(); //don't init here actually, it is done upon first opening
                    this->bookList.push_back(book);
                    indexedBooks[fileName] = book;

                    // Save immediately to LittleFS
                    saveBook(book);
                }
                else
                {
                    //if the epub loading is unsuccessful, we also store the epub as malformed and inform the user
                    dev.notificationHandler->drawErrorNotification(fileName);
                    book = new Book(fileName,
                                        fileName,
                                        std::string("error during parsing"),
                                        0,
                                        0,
                                        std::vector<int> {},
                                        currentRS);
                    book->badParse = true;
                    book->parseError = "EPUB load() returned false (invalid ZIP container or missing/corrupt content.opf)";
                    captureBookFileDiagnostics(fullPath, book->parseLog);
                    this->bookList.push_back(book);
                    indexedBooks[fileName] = book;
                    // Save immediately to LittleFS
                    saveBook(book);
                    vTaskDelay(100);
                }
                delete epub;
                rtc_clearCurrentlyParsing();
            }
        }

        if (dev.activeBookPath.empty()) dev.activeBookPath = fileName;
    }
    // Step 3: Prune metadata for missing books (not found in /sdcard)
const char *basePath =
    Device::getInstance().deviceSettings.storeDataOnSD
        ? "/sdcard/book_data"
        : "/littlefs/books";

for (auto it = indexedBooks.begin(); it != indexedBooks.end();) {
    if (std::find(foundPaths.begin(), foundPaths.end(), it->first) == foundPaths.end()) {

        if (it->second->badParse) {
            std::string metaFile =
                std::string(basePath) + "/" + getBookMetaFilename(it->first);

            if (unlink(metaFile.c_str()) == 0) {
                ESP_LOGI(TAG, "Pruned stale bad-parse metadata: %s", metaFile.c_str());
            } else {
                ESP_LOGW(TAG, "Failed to remove stale bad-parse metadata: %s", metaFile.c_str());
            }
        }

        dev.removeRecentBook(it->first);
        delete it->second;
        it = indexedBooks.erase(it);
    } else {
        ++it;
    }
}

    // Sync recentBooks if empty but indexed books have lastReadCounter
    if (dev.recentBooks.empty()) {
        std::vector<Book*> readBooks;
        for (const auto& pair : indexedBooks) {
            if (pair.second && pair.second->lastReadCounter > 0) {
                readBooks.push_back(pair.second);
            }
        }
        std::sort(readBooks.begin(), readBooks.end(), [](Book* a, Book* b) {
            return a->lastReadCounter > b->lastReadCounter;
        });
        for (size_t i = 0; i < readBooks.size() && i < Device::MAX_RECENT_BOOKS; ++i) {
            dev.recentBooks.push_back({readBooks[i]->path, readBooks[i]->lastReadCounter});
        }
        if (!dev.recentBooks.empty() && dev.bookReadCounter < dev.recentBooks[0].counter) {
            dev.bookReadCounter = dev.recentBooks[0].counter;
            dev.saveAppState();
        }
    }

    // Step 4: Sort books by author's last name, then group into authorList
    sortBooksByAuthorLastName(this->bookList);
    for (Book* b : bookList) {
        Author* foundAuthor = nullptr;
        int currentAuthorIndex = 0;
        for (auto& a : authorList) {
            if (a.name == b->author) {
                foundAuthor = &a;
                break;
            }
            currentAuthorIndex++;
        }

        if (foundAuthor) {
            foundAuthor->bookList.push_back(b);
            if (b->author == dev.activeAuthorName) {
                dev.activeAuthorIndex = currentAuthorIndex;
            }
        } else {
            authorList.emplace_back(b->author);
            authorList.back().bookList.push_back(b);
            if (b->author == dev.activeAuthorName) {
                dev.activeAuthorIndex = authorList.size() - 1;
            }
            foundAuthor = &authorList.back();
        }

        if (dev.activeBookPath == b->path) {
            dev.activeBookIndex = foundAuthor->bookList.size() - 1;
            if(dev.activeAuthorIndex==-1) //this book is the current book, but it's author doesn't match the current author, so it is in the favorites
            {
                dev.activeBookIndex = authorList[0].bookList.size(); //the book is the last entry in the favorites list, but not yet added, so size+1-1
                dev.activeAuthorIndex = 0;
                dev.activeAuthorName = authorList[0].name;
            }
        }

        if(b->favorite)
        {
            authorList[0].bookList.push_back(b);
        }
    }

    if(dev.state == Device::State::simpleReader) 
    {
        dev.activeBookIndex = 0;
        dev.activeAuthorIndex = 0;
        return; //if we boot into the simplereader we don't need to check the activebook
    }

    // Step 5: Fallback active indices if missing
    if (dev.activeBookIndex == -1 && !bookList.empty()) {
        dev.activeBookIndex = 0;
        dev.activeBookPath = bookList[0]->path;
    }
    if (dev.activeAuthorIndex == -1 && !authorList.empty()) {
        dev.activeAuthorIndex = 0;
        dev.activeAuthorName = authorList[0].name;
    }
}

void BookHandler::refreshFavorites()
{
    authorList[0].bookList.clear();
    for (Book* b : bookList) {
        if(b->favorite)
        {
            authorList[0].bookList.push_back(b);
        }
    }
}

void BookHandler::updateReadPage(const std::string &bookPath, int newPageCount) {
    auto it = indexedBooks.find(bookPath);
    if (it == indexedBooks.end()) return;

    Book *book = it->second;
    book->currentPage = newPageCount;

    saveBook(book); // only update this book's file
}

void BookHandler::reindexBook(Book *book) {
    if (!book) return;

    if (book->isComic()) {
        std::string fullPath = std::string("/sdcard/") + book->path;
        book->parseLog.clear();
        captureBookFileDiagnostics(fullPath, book->parseLog);
        if (book->isCbz()) {
            CbzArchive cbz(fullPath);
            if (cbz.isOpen() && cbz.getPageCount() > 0) {
                book->totalPageCount = cbz.getPageCount();
                book->badParse = false;
                book->parseError.clear();
                book->parseLog.clear();
            } else {
                book->badParse = true;
                book->parseError = "Reindex failed to open CBZ or found 0 image pages";
            }
        } else if (book->isCbr()) {
            CbrArchive cbr(fullPath);
            for (const std::string &step : cbr.getParseLog()) {
                book->parseLog.push_back(step);
            }
            if (cbr.isOpen() && cbr.getPageCount() > 0) {
                book->totalPageCount = cbr.getPageCount();
                book->badParse = false;
                book->parseError.clear();
                book->parseLog.clear();
            } else {
                book->badParse = true;
                book->parseError = cbr.getLastError().empty()
                    ? "Reindex failed to open CBR or found 0 image pages"
                    : cbr.getLastError();
            }
        }
        book->renderSettings = book->getCurrentRenderSettings();
        saveBook(book);
        ESP_LOGI(TAG, "Re-indexed and saved comic book: %s", book->title.c_str());
        return;
    }

    Reader reader;
    reader.init(book, nullptr);
    reader.indexPages();
    book->renderSettings = book->getCurrentRenderSettings();

    saveBook(book); // only update this book's file
    ESP_LOGI(TAG, "Re-indexed and saved book: %s", book->title.c_str());
}

