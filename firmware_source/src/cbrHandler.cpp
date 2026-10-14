#include "cbrHandler.h"
#include "cbzHandler.h"
#include "renderer.h"

#include "rar.hpp"

#include "esp_log.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>

extern void rtc_setParsingStep(const char *step);

static const char *TAG = "CBR";

struct CbrArchive::RarImpl {
    CommandData cmd;
    Archive arc;
    ComprDataIO dataIO;
    Unpack unpack;
    bool openFlag = false;
    int lastSolidOrder = -2;

    RarImpl() : arc(&cmd), unpack(&dataIO) {
        dataIO.EnableShowProgress(false);
        dataIO.SetSkipUnpCRC(true);
    }

    ~RarImpl() {
        if (openFlag) {
            arc.Close();
            openFlag = false;
        }
    }

    static RarImpl *createInPsram() {
        void *mem = allocComicPageBuffer(sizeof(RarImpl));
        if (!mem) {
            return nullptr;
        }
        return new (mem) RarImpl();
    }

    static void destroyInPsram(RarImpl *impl) {
        if (impl) {
            impl->~RarImpl();
            free(impl);
        }
    }

    static bool unpackSingleCurrentFileBlock(
        Archive &arc,
        ComprDataIO &dataIO,
        Unpack &unpack,
        uint8_t *destBuf,
        size_t destSize,
        bool solidFlag)
    {
        FileHeader &hd = arc.FileHead;
        int64 payloadOffset = arc.NextBlockPos - hd.PackSize;
        arc.Seek(payloadOffset, SEEK_SET);

        dataIO.Init();
        dataIO.EnableShowProgress(false);
        dataIO.SetSkipUnpCRC(true);
        dataIO.SetFiles(&arc, nullptr);
        dataIO.SetPackedSizeToRead(hd.PackSize);
        if (destBuf && destSize > 0) {
            dataIO.SetUnpackToMemory(destBuf, static_cast<uint>(destSize));
        } else {
            dataIO.SetTestMode(true);
        }

        if (hd.Method == 0) {
            // Method 0x30 (STORED): direct stream copy without LZ/PPMd dictionary
            if (destBuf && destSize > 0) {
                size_t remaining = destSize;
                uint8_t *p = destBuf;
                while (remaining > 0) {
                    int rd = dataIO.UnpRead(p, remaining);
                    if (rd <= 0) return false;
                    p += rd;
                    remaining -= static_cast<size_t>(rd);
                }
            }
            return true;
        }

        unpack.Init(hd.WinSize, solidFlag);
        unpack.SetDestSize(hd.UnpSize);
        unpack.DoUnpack(hd.UnpVer, solidFlag);
        return true;
    }

    bool extractBlockAtOffset(
        int64_t blockPos,
        int targetArchiveOrder,
        uint8_t *destBuf,
        size_t destSize)
    {
        jmp_buf env;
        g_unrar_jmp_env = &env;
        if (setjmp(env) != 0) {
            g_unrar_jmp_env = nullptr;
            lastSolidOrder = -2;
            return false;
        }

        ErrHandler.Clean();

        // Fast path (99%+ of .cbr files): non-solid archive -> O(1) direct random-access file seek!
        if (!arc.Solid) {
            arc.Seek(blockPos, SEEK_SET);
            if (arc.ReadHeader() <= 0 || arc.GetHeaderType() != HEAD_FILE) {
                g_unrar_jmp_env = nullptr;
                return false;
            }
            bool ok = unpackSingleCurrentFileBlock(
                arc, dataIO, unpack, destBuf, destSize, arc.FileHead.Solid);
            g_unrar_jmp_env = nullptr;
            return ok;
        }

        // Solid RAR archive fallback: maintain sliding window across sequential solid entries
        if (lastSolidOrder + 1 != targetArchiveOrder) {
            arc.Seek(0, SEEK_SET);
            lastSolidOrder = -1;
        }

        int currentOrder = -1;
        while (arc.SearchBlock(HEAD_FILE) > 0) {
            ++currentOrder;
            if (currentOrder <= lastSolidOrder) {
                arc.SeekToNext();
                continue;
            }
            bool isTarget = (currentOrder == targetArchiveOrder);
            bool ok = unpackSingleCurrentFileBlock(
                arc,
                dataIO,
                unpack,
                isTarget ? destBuf : nullptr,
                isTarget ? destSize : 0,
                arc.FileHead.Solid);
            lastSolidOrder = currentOrder;
            if (isTarget) {
                g_unrar_jmp_env = nullptr;
                return ok;
            }
            arc.SeekToNext();
        }

        g_unrar_jmp_env = nullptr;
        return false;
    }
};

CbrArchive::CbrArchive() : m_rar(RarImpl::createInPsram()) {}

CbrArchive::CbrArchive(const std::string &fullPath) : m_rar(RarImpl::createInPsram()) {
    open(fullPath);
}

CbrArchive::~CbrArchive() {
    close();
    RarImpl::destroyInPsram(m_rar);
    m_rar = nullptr;
}

void CbrArchive::close() {
    m_pages.clear();
    if (m_rar && m_rar->openFlag) {
        m_rar->arc.Close();
        m_rar->openFlag = false;
        m_rar->lastSolidOrder = -2;
    }
}

bool CbrArchive::isOpen() const {
    return m_rar && m_rar->openFlag;
}

bool CbrArchive::open(const std::string &fullPath) {
    close();
    m_path = fullPath;
    m_lastError.clear();
    m_parseLog.clear();

    m_parseLog.push_back("open_path=" + fullPath);

    if (!m_rar) {
        m_lastError = "Failed to allocate RarImpl (" + std::to_string(sizeof(RarImpl)) + " bytes in PSRAM)";
        m_parseLog.push_back("error: " + m_lastError);
        ESP_LOGE(TAG, "%s: %s", m_lastError.c_str(), fullPath.c_str());
        return false;
    }
    m_parseLog.push_back("rar_impl_allocated_bytes=" + std::to_string(sizeof(RarImpl)));

    jmp_buf env;
    g_unrar_jmp_env = &env;
    if (setjmp(env) != 0) {
        g_unrar_jmp_env = nullptr;
        int errCode = ErrHandler.GetErrorCode();
        m_lastError = "UnRAR longjmp abort during open (error_code=" + std::to_string(errCode) + ")";
        m_parseLog.push_back("error: " + m_lastError);
        ESP_LOGE(TAG, "%s: %s", m_lastError.c_str(), fullPath.c_str());
        return false;
    }

    ErrHandler.Clean();

    rtc_setParsingStep("cbr:utf_to_wide");
    std::wstring wPath;
    UtfToWide(fullPath.c_str(), wPath);
    std::string roundTripPath;
    WideToChar(wPath, roundTripPath);
    m_parseLog.push_back("wpath_len=" + std::to_string(wPath.size()) + " roundtrip=" + roundTripPath);

    rtc_setParsingStep("cbr:arc_open");
    errno = 0;
    if (!m_rar->arc.Open(wPath, 0)) {
        int savedErr = errno;
        g_unrar_jmp_env = nullptr;
        m_lastError = "Archive::Open failed (errno=" + std::to_string(savedErr) + ": " + std::string(strerror(savedErr)) + ")";
        m_parseLog.push_back("error: " + m_lastError);
        ESP_LOGE(TAG, "%s for %s", m_lastError.c_str(), fullPath.c_str());
        return false;
    }
    m_parseLog.push_back("arc_open=ok file_len=" + std::to_string(static_cast<long long>(m_rar->arc.FileLength())));

    rtc_setParsingStep("cbr:is_archive");
    if (!m_rar->arc.IsArchive(true)) {
        g_unrar_jmp_env = nullptr;
        char diagBuf[192];
        snprintf(diagBuf, sizeof(diagBuf),
                 "format=%d solid=%d encrypted=%d broken=%d failed_decrypt=%d sfx=%lld",
                 static_cast<int>(m_rar->arc.Format),
                 static_cast<int>(m_rar->arc.Solid),
                 static_cast<int>(m_rar->arc.Encrypted),
                 static_cast<int>(m_rar->arc.BrokenHeader),
                 static_cast<int>(m_rar->arc.FailedHeaderDecryption),
                 static_cast<long long>(m_rar->arc.SFXSize));
        m_rar->arc.Close();
        m_lastError = std::string("Archive::IsArchive(true) returned false (") + diagBuf + ")";
        m_parseLog.push_back("error: " + m_lastError);
        ESP_LOGE(TAG, "%s: %s", m_lastError.c_str(), fullPath.c_str());
        return false;
    }
    m_rar->openFlag = true;
    {
        char hdrInfo[160];
        snprintf(hdrInfo, sizeof(hdrInfo),
                 "is_archive=ok format=%d solid=%d encrypted=%d broken=%d sfx=%lld",
                 static_cast<int>(m_rar->arc.Format),
                 static_cast<int>(m_rar->arc.Solid),
                 static_cast<int>(m_rar->arc.Encrypted),
                 static_cast<int>(m_rar->arc.BrokenHeader),
                 static_cast<long long>(m_rar->arc.SFXSize));
        m_parseLog.push_back(hdrInfo);
    }

    int64_t comicInfoPos = -1;
    int comicInfoOrder = -1;
    size_t comicInfoSize = 0;

    rtc_setParsingStep("cbr:scan_headers");
    int order = 0;
    int dirSkipped = 0;
    int encSkipped = 0;
    while (m_rar->arc.SearchBlock(HEAD_FILE) > 0) {
        FileHeader &hd = m_rar->arc.FileHead;
        int currentOrder = order++;

        if (hd.Dir || hd.Encrypted) {
            if (hd.Dir) ++dirSkipped;
            if (hd.Encrypted) ++encSkipped;
            m_rar->arc.SeekToNext();
            continue;
        }

        std::string utf8Name;
        WideToUtf(hd.FileName.c_str(), utf8Name);

        size_t slash = utf8Name.find_last_of("/\\");
        std::string base = (slash == std::string::npos) ? utf8Name : utf8Name.substr(slash + 1);
        std::string lowerBase = base;
        for (char &c : lowerBase) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        if (lowerBase == "comicinfo.xml" && hd.UnpSize > 0 && hd.UnpSize <= 256 * 1024) {
            comicInfoPos = m_rar->arc.CurBlockPos;
            comicInfoOrder = currentOrder;
            comicInfoSize = static_cast<size_t>(hd.UnpSize);
            m_rar->arc.SeekToNext();
            continue;
        }

        if (isComicImageEntry(utf8Name) && hd.UnpSize > 0) {
            m_pages.push_back({
                m_rar->arc.CurBlockPos,
                currentOrder,
                std::move(utf8Name),
                static_cast<size_t>(hd.UnpSize),
                hd.Solid
            });
        }
        m_rar->arc.SeekToNext();
    }

    std::sort(m_pages.begin(), m_pages.end(), [](const PageEntry &a, const PageEntry &b) {
        return naturalLessComicEntry(a.name, b.name);
    });

    {
        char scanInfo[192];
        snprintf(scanInfo, sizeof(scanInfo),
                 "scan_done total_blocks=%d pages=%zu dirs=%d encrypted=%d comicinfo=%d",
                 order, m_pages.size(), dirSkipped, encSkipped, (comicInfoPos >= 0 ? 1 : 0));
        m_parseLog.push_back(scanInfo);
    }

    m_title = defaultComicTitleFromPath(fullPath);
    m_author = "Comic Book";

    g_unrar_jmp_env = nullptr;

    if (comicInfoPos >= 0 && comicInfoSize > 0) {
        rtc_setParsingStep("cbr:comicinfo_xml");
        std::vector<uint8_t> xmlBuf(comicInfoSize + 1, 0);
        if (m_rar->extractBlockAtOffset(comicInfoPos, comicInfoOrder, xmlBuf.data(), comicInfoSize)) {
            parseComicInfoXml(reinterpret_cast<const char *>(xmlBuf.data()), comicInfoSize, m_title, m_author);
        }
    }

    if (m_pages.empty()) {
        m_lastError = "Archive opened (format=" + std::to_string(static_cast<int>(m_rar->arc.Format)) +
                      "), scanned " + std::to_string(order) +
                      " file headers, but found 0 supported image pages (.jpg/.jpeg/.png); encrypted_skipped=" +
                      std::to_string(encSkipped);
        m_parseLog.push_back("error: " + m_lastError);
    }

    ESP_LOGI(TAG, "Opened CBR '%s' (%zu image pages)", m_title.c_str(), m_pages.size());
    return !m_pages.empty();
}

bool CbrArchive::extractPageToBuffer(int pageIndex, uint8_t *outBuf, size_t outSize) {
    if (!isOpen() || !outBuf || outSize == 0) {
        return false;
    }
    if (pageIndex < 0 || pageIndex >= static_cast<int>(m_pages.size())) {
        return false;
    }

    const PageEntry &entry = m_pages[pageIndex];
    return m_rar->extractBlockAtOffset(entry.blockPos, entry.archiveOrder, outBuf, outSize);
}

bool CbrArchive::renderPage(int pageIndex, Renderer *renderer, unsigned char *targetFrameBuffer) {
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
        ESP_LOGE(TAG, "Failed to allocate %zu bytes for CBR page %d (%s)",
                 entry.uncompSize, pageIndex, entry.name.c_str());
        return false;
    }

    if (!extractPageToBuffer(pageIndex, rawImg, entry.uncompSize)) {
        ESP_LOGE(TAG, "Failed to unpack CBR page %d (%s)", pageIndex, entry.name.c_str());
        free(rawImg);
        return false;
    }

    return decodeAndRenderComicPageBuffer(rawImg, entry.uncompSize, entry.name, renderer, targetFrameBuffer);
}

bool CbrArchive::loadPageZoomCache(int pageIndex, ComicZoomPageCache &outCache) {
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
        ESP_LOGE(TAG, "Failed to allocate %zu bytes for CBR zoom cache page %d (%s)",
                 entry.uncompSize, pageIndex, entry.name.c_str());
        return false;
    }

    if (!extractPageToBuffer(pageIndex, rawImg, entry.uncompSize)) {
        ESP_LOGE(TAG, "Failed to unpack CBR zoom cache page %d (%s)", pageIndex, entry.name.c_str());
        free(rawImg);
        return false;
    }

    return decodeComicPageToZoomCache(rawImg, entry.uncompSize, entry.name, pageIndex, outCache);
}
