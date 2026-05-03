// renderer.cpp -- Layout and painting for MDViewer.
//
// Strategy:
//   * Layout() walks the parsed Document and produces a flat list of
//     positioned DrawOp records (text runs, images, rectangles, lines).
//   * Paint() iterates that list and draws only the parts that overlap
//     the visible Y window. Text uses GDI ExtTextOutW with cached fonts.
//   * Images are loaded once via GDI+, converted to GDI HBITMAPs, and
//     drawn with StretchBlt.
//   * Search highlights are overlaid AFTER the corresponding text is
//     drawn (we paint the highlight rect first, then redraw the text on
//     top -- not the other way around -- to keep glyphs readable).
//
// Limitations:
//   * Text selection by drag is intentionally NOT implemented; "select
//     all + Ctrl+C" copies the whole plain-text representation instead.
//   * Lists are rendered single-level (no nested indent levels).

#include "renderer.h"

#include <windows.h>
#include <gdiplus.h>
#include <d2d1.h>
#include <dwrite.h>
#include <winhttp.h>
#include <algorithm>
#include <climits>
#include <cwctype>
#include <cstring>

// The D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT flag was added in the
// Windows 8.1 SDK. AlmaLinux 8's mingw-w64 7.2 still ships pre-8.1
// headers, so we fall back to the documented numeric value when the
// symbol is missing. The bit is silently ignored on older Windows
// versions, so this is a header-only fix.
#ifndef D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT
#define D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT \
    static_cast<D2D1_DRAW_TEXT_OPTIONS>(0x00000004)
#endif

#ifdef _MSC_VER
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "winhttp.lib")
#endif

namespace md {

// ---- Color palette --------------------------------------------------------
static const COLORREF C_TEXT     = RGB(36, 41, 47);     // body
static const COLORREF C_MUTED    = RGB(101, 109, 118);  // blockquote
static const COLORREF C_LINK     = RGB(9, 105, 218);
static const COLORREF C_CODE_BG  = RGB(238, 240, 243);  // inline code background
static const COLORREF C_BLOCK_BG = RGB(246, 248, 250);  // code block bg / table header
static const COLORREF C_BORDER   = RGB(208, 215, 222);
static const COLORREF C_RULE     = RGB(216, 222, 228);
static const COLORREF C_BG       = RGB(255, 255, 255);  // page bg
static const COLORREF C_HL       = RGB(255, 245, 157);  // search match
static const COLORREF C_HL_CUR   = RGB(255, 138, 101);  // current match
static const COLORREF C_HL_CUR_BORDER = RGB(216, 76, 35);

// ---- Sentinel for "no background" ----------------------------------------
static constexpr COLORREF NO_BG = 0xFFFFFFFF;

void Renderer::SetZoom(float z) {
    if (z < 0.5f) z = 0.5f;
    if (z > 3.0f) z = 3.0f;
    if (std::abs(z - zoom_) < 0.001f) return;
    zoom_ = z;
    DestroyFonts();   // force rebuild at new size on next Layout()
}

Renderer::Renderer() {}
Renderer::~Renderer() {
    DestroyFonts();
    DestroyImages();
}

void Renderer::SetDocument(Document doc) {
    doc_ = std::move(doc);
    basePath_ = doc_.basePath;
    DestroyImages();
    ops_.clear();
    hits_.clear();
    anchors_.clear();
    currentHit_ = -1;
    totalHeight_ = 0;
}

void Renderer::DestroyFonts() {
    // Avoid double-free when FK_EMOJI fell back to FK_BODY's font.
    Gdiplus::Font* sharedBody = gpFonts_[FK_BODY];
    for (int i = 0; i < FK_COUNT; ++i) {
        if (i != FK_BODY && gpFonts_[i] == sharedBody) {
            gpFonts_[i] = nullptr;
            continue;
        }
        if (gpFonts_[i]) { delete gpFonts_[i]; gpFonts_[i] = nullptr; }
    }
    if (familyBody_)  { delete familyBody_;  familyBody_ = nullptr; }
    if (familyMono_)  { delete familyMono_;  familyMono_ = nullptr; }
    if (familyEmoji_) { delete familyEmoji_; familyEmoji_ = nullptr; }
    if (dwEmojiFormat_) {
        ((IDWriteTextFormat*)dwEmojiFormat_)->Release();
        dwEmojiFormat_ = nullptr;
    }
}

void Renderer::DestroyImages() {
    for (auto& e : images_) {
        if (e.bmp)       { delete e.bmp; e.bmp = nullptr; }
        if (e.cachedHbm) { DeleteObject(e.cachedHbm); e.cachedHbm = nullptr; }
    }
    images_.clear();
}

// ---------- Font creation --------------------------------------------------
// Pull GDI+ in early so EnsureGdiplus is initialised before we create
// FontFamily / Font instances.
static ULONG_PTR g_gdiplusToken = 0;
static int       g_gdiplusRefcount = 0;

static void EnsureGdiplus() {
    if (g_gdiplusRefcount++ == 0) {
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&g_gdiplusToken, &input, nullptr);
    }
}
[[maybe_unused]] static void ReleaseGdiplus() {
    if (--g_gdiplusRefcount == 0) {
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
    }
}

// ---- Direct2D + DirectWrite (for color emoji rendering) ------------------
// GDI+ does not honour COLR/CPAL color font tables, so emoji from
// "Segoe UI Emoji" come out monochrome. Direct2D's DrawText with
// D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT renders them properly.
static ID2D1Factory*    g_d2dFactory    = nullptr;
static IDWriteFactory*  g_dwriteFactory = nullptr;

static void EnsureDirect2D() {
    if (!g_d2dFactory) {
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                          __uuidof(ID2D1Factory),
                          (void**)&g_d2dFactory);
    }
    if (!g_dwriteFactory) {
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                            __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(&g_dwriteFactory));
    }
}

static Gdiplus::FontFamily* MakeFamily(const wchar_t* primary, const wchar_t* fallback) {
    Gdiplus::FontFamily* fam = new Gdiplus::FontFamily(primary);
    if (fam->GetLastStatus() != Gdiplus::Ok) {
        delete fam;
        fam = new Gdiplus::FontFamily(fallback);
    }
    return fam;
}

void Renderer::CreateFonts(HWND hwnd) {
    DestroyFonts();
    EnsureGdiplus();

    HDC hdc = GetDC(hwnd);
    int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
    auto px = [dpi, this](double pt) { return (float)(pt * dpi / 72.0 * zoom_); };

    familyBody_ = MakeFamily(L"Segoe UI",  L"Tahoma");
    familyMono_ = MakeFamily(L"Consolas", L"Courier New");

    auto setKind = [&](int kind, Gdiplus::FontFamily* fam, float sizePx, int gpStyle) {
        fontSizePx_[kind] = sizePx;
        gpFonts_[kind] = new Gdiplus::Font(fam, sizePx, gpStyle, Gdiplus::UnitPixel);
    };

    float body = (float)((int)(px(11.0) + 0.5));
    float code = (float)((int)(px(10.0) + 0.5));

    setKind(FK_BODY,    familyBody_, body, Gdiplus::FontStyleRegular);
    setKind(FK_BODY_B,  familyBody_, body, Gdiplus::FontStyleBold);
    setKind(FK_BODY_I,  familyBody_, body, Gdiplus::FontStyleItalic);
    setKind(FK_BODY_BI, familyBody_, body, Gdiplus::FontStyleBoldItalic);
    setKind(FK_CODE,    familyMono_, code, Gdiplus::FontStyleRegular);
    setKind(FK_CODE_B,  familyMono_, code, Gdiplus::FontStyleBold);
    setKind(FK_CODE_I,  familyMono_, code, Gdiplus::FontStyleItalic);
    setKind(FK_H1,      familyBody_, (float)(int)(px(22)+0.5), Gdiplus::FontStyleBold);
    setKind(FK_H2,      familyBody_, (float)(int)(px(18)+0.5), Gdiplus::FontStyleBold);
    setKind(FK_H3,      familyBody_, (float)(int)(px(15)+0.5), Gdiplus::FontStyleBold);
    setKind(FK_H4,      familyBody_, (float)(int)(px(12)+0.5), Gdiplus::FontStyleBold);
    setKind(FK_H5,      familyBody_, (float)(int)(px(11)+0.5), Gdiplus::FontStyleBold);
    setKind(FK_H6,      familyBody_, (float)(int)(px(10)+0.5), Gdiplus::FontStyleBold);

    // Emoji font: explicit Segoe UI Emoji so color glyphs render through
    // GDI+'s DirectWrite path on Windows 10+. We use this as a per-run
    // override for any text that contains emoji codepoints.
    Gdiplus::FontFamily* familyEmoji = new Gdiplus::FontFamily(L"Segoe UI Emoji");
    if (familyEmoji->GetLastStatus() != Gdiplus::Ok) {
        delete familyEmoji;
        // Fallback to whatever we have for body — at least it will render
        // monochrome glyphs from the BMP.
        familyEmoji = new Gdiplus::FontFamily(L"Segoe UI Symbol");
        if (familyEmoji->GetLastStatus() != Gdiplus::Ok) {
            delete familyEmoji;
            familyEmoji = nullptr;
        }
    }
    fontSizePx_[FK_EMOJI] = body;
    if (familyEmoji) {
        // Note: we leak this FontFamily on subsequent CreateFonts calls;
        // it lives until DLL shutdown. Acceptable trade-off vs storing
        // an extra member.
        gpFonts_[FK_EMOJI] = new Gdiplus::Font(familyEmoji, body,
                                               Gdiplus::FontStyleRegular,
                                               Gdiplus::UnitPixel);
        // Stash the family pointer so DestroyFonts cleans it up alongside
        // the others on the next rebuild.
        if (familyEmoji_) delete familyEmoji_;
        familyEmoji_ = familyEmoji;
    } else {
        gpFonts_[FK_EMOJI] = gpFonts_[FK_BODY];   // fallback share
    }

    // Pre-compute pixel metrics from each font's family + style.
    for (int i = 0; i < FK_COUNT; ++i) {
        Gdiplus::Font* f = gpFonts_[i];
        if (!f) continue;
        Gdiplus::FontFamily fam;
        f->GetFamily(&fam);
        int style = f->GetStyle();
        UINT16 emHeight     = fam.GetEmHeight(style);
        UINT16 cellAscent   = fam.GetCellAscent(style);
        UINT16 cellDescent  = fam.GetCellDescent(style);
        UINT16 lineSpacing  = fam.GetLineSpacing(style);
        if (emHeight == 0) emHeight = 1;
        float ratio   = fontSizePx_[i] / (float)emHeight;
        ascent_[i]    = (int)(cellAscent  * ratio + 0.5f);
        descent_[i]   = (int)(cellDescent * ratio + 0.5f);
        lineHeight_[i] = (int)(lineSpacing * ratio + 0.5f);
        avgChar_[i]   = (int)(fontSizePx_[i] * 0.5f);
    }

    // Build a DirectWrite text format for emoji glyphs at the body size.
    EnsureDirect2D();
    if (dwEmojiFormat_) {
        ((IDWriteTextFormat*)dwEmojiFormat_)->Release();
        dwEmojiFormat_ = nullptr;
    }
    if (g_dwriteFactory) {
        IDWriteTextFormat* fmt = nullptr;
        // DirectWrite uses DIPs (1/96 inch) for the size argument; our
        // fontSizePx_ values are already in physical pixels at the screen
        // DPI, which equals DIPs at 96 DPI. Most TC users run at 96 DPI;
        // for HiDPI we'd need to scale. Acceptable simplification.
        if (g_dwriteFactory->CreateTextFormat(
                L"Segoe UI Emoji", nullptr,
                DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                fontSizePx_[FK_EMOJI],
                L"", &fmt) == S_OK && fmt) {
            fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            dwEmojiFormat_ = fmt;
        }
    }

    ReleaseDC(hwnd, hdc);
}

Gdiplus::Font* Renderer::GetGdiplusFont_Public(int idx) const {
    if (idx < 0 || idx >= FK_COUNT) return nullptr;
    return gpFonts_[idx];
}

// ---------- Image loading via GDI+ ----------------------------------------

static std::wstring UrlDecodeW(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    auto hexv = [](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return 10 + (c - L'a');
        if (c >= L'A' && c <= L'F') return 10 + (c - L'A');
        return -1;
    };
    // Detect URL-style escapes only if they appear plausibly
    // (don't mangle paths with literal % signs).
    std::string utf8;
    bool anyEscape = false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'%' && i + 2 < s.size()) {
            int a = hexv(s[i+1]);
            int b = hexv(s[i+2]);
            if (a >= 0 && b >= 0) { anyEscape = true; break; }
        }
    }
    if (!anyEscape) return s;

    for (size_t i = 0; i < s.size(); ) {
        if (s[i] == L'%' && i + 2 < s.size()) {
            int a = hexv(s[i+1]);
            int b = hexv(s[i+2]);
            if (a >= 0 && b >= 0) {
                utf8.push_back((char)((a << 4) | b));
                i += 3;
                continue;
            }
        }
        // Non-escape: encode as UTF-8 byte(s)
        wchar_t c = s[i++];
        if (c < 0x80) utf8.push_back((char)c);
        else if (c < 0x800) {
            utf8.push_back((char)(0xC0 | (c >> 6)));
            utf8.push_back((char)(0x80 | (c & 0x3F)));
        } else {
            utf8.push_back((char)(0xE0 | (c >> 12)));
            utf8.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
            utf8.push_back((char)(0x80 | (c & 0x3F)));
        }
    }
    int wn = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    if (wn <= 0) return s;
    std::wstring w; w.resize(wn);
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), w.data(), wn);
    return w;
}

// ===========================================================================
//  Remote image fetching (http / https) with on-disk cache
// ---------------------------------------------------------------------------
// Markdown often references badges and screenshots from the web — shields.io
// most famously. We download those resources on the fly using WinHTTP and
// cache them to %TEMP%\MDViewer\cache so that the second open of the same
// document is instant. PrefetchRemoteImages spawns one OS thread per URL so
// many badges download concurrently rather than serially.
// ===========================================================================

static uint64_t Fnv1a64Hash(const std::wstring& s) {
    uint64_t h = 14695981039346656037ULL;
    for (wchar_t c : s) {
        unsigned int v = (unsigned int)c;
        h ^= (uint64_t)(v & 0xFF);        h *= 1099511628211ULL;
        h ^= (uint64_t)((v >> 8) & 0xFF); h *= 1099511628211ULL;
    }
    return h;
}

static std::wstring GetUrlCacheDir() {
    wchar_t tmp[MAX_PATH] = {};
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (n == 0 || n >= MAX_PATH) return L"";
    std::wstring base = std::wstring(tmp) + L"MDViewer";
    CreateDirectoryW(base.c_str(), nullptr);
    base += L"\\cache";
    CreateDirectoryW(base.c_str(), nullptr);
    return base;
}

static std::wstring CachePathForUrl(const std::wstring& url) {
    std::wstring dir = GetUrlCacheDir();
    if (dir.empty()) return L"";
    wchar_t buf[24];
    _snwprintf(buf, 24, L"%016llx", (unsigned long long)Fnv1a64Hash(url));
    return dir + L"\\" + buf + L".dat";
}

// Some badge services (shields.io being the canonical example) serve SVG by
// default and we have no SVG renderer. shields.io conveniently accepts a
// `.png` suffix on the badge path to return a raster image, so transparently
// rewrite those URLs at fetch time only — the cache key stays the original
// URL so the user can change their mind without invalidating local copies.
static std::wstring CanonicaliseUrlForFetch(const std::wstring& url) {
    if (url.find(L"img.shields.io/") != std::wstring::npos) {
        size_t qmark = url.find(L'?');
        std::wstring path = (qmark == std::wstring::npos) ? url : url.substr(0, qmark);
        std::wstring tail = (qmark == std::wstring::npos) ? L"" : url.substr(qmark);
        bool hasPng = (path.size() >= 4
                       && _wcsicmp(path.c_str() + path.size() - 4, L".png") == 0);
        bool hasSvg = (path.size() >= 4
                       && _wcsicmp(path.c_str() + path.size() - 4, L".svg") == 0);
        if (!hasPng) {
            if (hasSvg) path = path.substr(0, path.size() - 4);
            return path + L".png" + tail;
        }
    }
    return url;
}

// Synchronous download. Returns true only on a 200 response with bytes
// successfully written to outPath. On any failure the partially-written file
// is removed so the next attempt re-downloads.
static bool DownloadUrlToFile(const std::wstring& url, const std::wstring& outPath) {
    std::wstring fetchUrl = CanonicaliseUrlForFetch(url);
    URL_COMPONENTS uc = {};
    uc.dwStructSize     = sizeof(uc);
    wchar_t hostBuf[256] = {};
    wchar_t pathBuf[2048] = {};
    uc.lpszHostName     = hostBuf; uc.dwHostNameLength = 256;
    uc.lpszUrlPath      = pathBuf; uc.dwUrlPathLength  = 2048;
    if (!WinHttpCrackUrl(fetchUrl.c_str(), 0, 0, &uc)) return false;

    HINTERNET hSession = WinHttpOpen(L"MDViewer/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    WinHttpSetTimeouts(hSession, 3000, 3000, 3000, 5000);

    HINTERNET hConnect = WinHttpConnect(hSession, hostBuf, uc.nPort, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

    DWORD reqFlags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET", pathBuf,
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, reqFlags);
    if (!hReq) {
        WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return false;
    }
    const wchar_t* acceptHdr =
        L"Accept: image/png, image/webp, image/jpeg, image/gif, image/*;q=0.8\r\n";
    WinHttpAddRequestHeaders(hReq, acceptHdr, -1L,
        WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    bool ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
              && WinHttpReceiveResponse(hReq, nullptr);
    if (ok) {
        DWORD status = 0, ssz = sizeof(status);
        WinHttpQueryHeaders(hReq,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &ssz, WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            HANDLE hOut = CreateFileW(outPath.c_str(),
                GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hOut == INVALID_HANDLE_VALUE) {
                ok = false;
            } else {
                BYTE buf[8192];
                size_t total = 0;
                const size_t MAX_SIZE = 50 * 1024 * 1024;
                for (;;) {
                    DWORD got = 0;
                    if (!WinHttpReadData(hReq, buf, sizeof(buf), &got)) { ok = false; break; }
                    if (got == 0) break;
                    DWORD wrote = 0;
                    if (!WriteFile(hOut, buf, got, &wrote, nullptr) || wrote != got) {
                        ok = false; break;
                    }
                    total += got;
                    if (total > MAX_SIZE) { ok = false; break; }
                }
                CloseHandle(hOut);
                if (!ok) DeleteFileW(outPath.c_str());
            }
        } else {
            ok = false;
        }
    }
    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok;
}

namespace {
struct DownloadCtx {
    std::wstring url;
    std::wstring path;
};
DWORD WINAPI DownloadThreadProc(LPVOID lp) {
    DownloadCtx* ctx = (DownloadCtx*)lp;
    DownloadUrlToFile(ctx->url, ctx->path);
    delete ctx;
    return 0;
}
} // namespace

// Read the natural pixel dimensions of an image directly from its file
// header — i.e. without decoding any pixel data. Supports the formats we
// see in real-world READMEs (PNG, GIF, JPEG, BMP). Returns false for
// unsupported formats; the caller then falls back to GDI+.
//
// This dance exists because Gdiplus::Bitmap::FromFile DOES decode the
// first frame, which for a 17 MB GIF takes 200–500 ms. Multiplied across
// every image in a long README, that cost dominates ListLoad.
static bool ReadImageDimensionsFast(const std::wstring& path,
                                    int* outW, int* outH) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    BYTE buf[2048];
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, sizeof(buf), &got, nullptr);
    CloseHandle(h);
    if (!ok || got < 24) return false;

    // PNG: 8-byte signature, then IHDR chunk; width/height at offset 16/20
    // in big-endian.
    if (buf[0] == 0x89 && buf[1] == 'P' && buf[2] == 'N' && buf[3] == 'G'
        && buf[4] == 0x0D && buf[5] == 0x0A && buf[6] == 0x1A && buf[7] == 0x0A) {
        int w = (buf[16] << 24) | (buf[17] << 16) | (buf[18] << 8) | buf[19];
        int hh = (buf[20] << 24) | (buf[21] << 16) | (buf[22] << 8) | buf[23];
        if (w > 0 && hh > 0) { *outW = w; *outH = hh; return true; }
        return false;
    }
    // GIF: "GIF87a" / "GIF89a", logical screen descriptor immediately
    // follows; width/height are 16-bit little-endian at offset 6 / 8.
    if (buf[0] == 'G' && buf[1] == 'I' && buf[2] == 'F'
        && buf[3] == '8' && (buf[4] == '7' || buf[4] == '9') && buf[5] == 'a') {
        int w  = (int)(buf[6] | (buf[7] << 8));
        int hh = (int)(buf[8] | (buf[9] << 8));
        if (w > 0 && hh > 0) { *outW = w; *outH = hh; return true; }
        return false;
    }
    // JPEG: scan markers until we hit a Start-Of-Frame; height/width are
    // big-endian 16-bit values inside SOFn segments.
    if (buf[0] == 0xFF && buf[1] == 0xD8) {
        DWORD i = 2;
        while (i + 9 < got) {
            if (buf[i] != 0xFF) return false;
            BYTE marker = buf[i + 1];
            // Pad bytes (FF FF) and standalone markers without payload.
            if (marker == 0xFF) { ++i; continue; }
            if (marker == 0x00 || marker == 0xD9) return false;
            // SOI/RSTn have no length field and we shouldn't see them
            // again here.
            if (marker >= 0xD0 && marker <= 0xD7) { i += 2; continue; }
            if (marker == 0xD8) { i += 2; continue; }
            DWORD segLen = (DWORD)((buf[i + 2] << 8) | buf[i + 3]);
            // SOFn = C0..CF except DHT(C4), JPG(C8), DAC(CC).
            bool isSOF = (marker >= 0xC0 && marker <= 0xCF
                          && marker != 0xC4 && marker != 0xC8 && marker != 0xCC);
            if (isSOF) {
                if (i + 9 >= got) return false;
                int hh = (int)((buf[i + 5] << 8) | buf[i + 6]);
                int w  = (int)((buf[i + 7] << 8) | buf[i + 8]);
                if (w > 0 && hh > 0) { *outW = w; *outH = hh; return true; }
                return false;
            }
            i += 2 + segLen;
        }
        return false;
    }
    // BMP: BITMAPFILEHEADER (14 bytes) + BITMAPINFOHEADER. Width/height
    // are little-endian INT32 at offset 18/22.
    if (buf[0] == 'B' && buf[1] == 'M' && got >= 26) {
        int w = (int)((DWORD)buf[18] | ((DWORD)buf[19] << 8)
                    | ((DWORD)buf[20] << 16) | ((DWORD)buf[21] << 24));
        int hh = (int)((DWORD)buf[22] | ((DWORD)buf[23] << 8)
                     | ((DWORD)buf[24] << 16) | ((DWORD)buf[25] << 24));
        if (hh < 0) hh = -hh;     // top-down DIB
        if (w > 0 && hh > 0) { *outW = w; *outH = hh; return true; }
    }
    return false;
}

int Renderer::LoadImageFromFile(const std::wstring& path) {
    if (path.empty()) return -1;

    std::wstring p = path;
    auto starts_iw = [&](const wchar_t* pfx) {
        size_t n = wcslen(pfx);
        if (p.size() < n) return false;
        for (size_t i = 0; i < n; ++i) {
            if (towlower(p[i]) != towlower(pfx[i])) return false;
        }
        return true;
    };
    if (starts_iw(L"data:")) return -1;     // not supported

    if (starts_iw(L"http://") || starts_iw(L"https://")) {
        // Remote image. Substitute the on-disk cache path; if the file
        // hasn't been prefetched yet, do a synchronous fetch as a last
        // resort. For typical openings PrefetchRemoteImages has already
        // populated the cache, so this path is fast.
        std::wstring cp = CachePathForUrl(p);
        if (cp.empty()) return -1;
        if (GetFileAttributesW(cp.c_str()) == INVALID_FILE_ATTRIBUTES) {
            if (!DownloadUrlToFile(p, cp)) return -1;
        }
        p = cp;
        // No URL-decode / "./" stripping for cache paths.
    } else {
        if (starts_iw(L"file://")) {
            p.erase(0, 7);
            if (!p.empty() && p[0] == L'/' && p.size() >= 3 && p[2] == L':') p.erase(0, 1);
        }
        p = UrlDecodeW(p);
        while (p.size() >= 2 && p[0] == L'.' && (p[1] == L'/' || p[1] == L'\\')) p.erase(0, 2);
        for (wchar_t& c : p) if (c == L'/') c = L'\\';
    }

    // Existence check via attributes — fast and avoids GDI+ logging.
    DWORD attr = GetFileAttributesW(p.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        return -1;
    }

    // Fast path: read just the dimensions from the file header. For PNG
    // and GIF this is a single read of the first ~24 bytes — orders of
    // magnitude faster than Bitmap::FromFile, which decodes frame 0.
    int natW = 0, natH = 0;
    if (ReadImageDimensionsFast(p, &natW, &natH)) {
        ImageEntry entry;
        entry.path   = p;
        entry.width  = natW;
        entry.height = natH;
        entry.totalFrames = 1;
        entry.loaded = false;
        int idx = (int)images_.size();
        images_.push_back(std::move(entry));
        return idx;
    }

    // Slow path: format we don't recognise (rare). Fall back to a full
    // Gdiplus decode just to get dimensions. The decoded Bitmap is then
    // kept so EnsureImageRenderedAt_Public can use it later.
    EnsureGdiplus();
    int idx = -1;
    Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromFile(p.c_str(), FALSE);
    if (bmp && bmp->GetLastStatus() == Gdiplus::Ok) {
        UINT w = bmp->GetWidth();
        UINT h = bmp->GetHeight();
        if (w > 0 && h > 0) {
            ImageEntry entry;
            entry.path   = p;
            entry.width  = (int)w;
            entry.height = (int)h;
            entry.totalFrames = 1;
            entry.bmp    = bmp;
            entry.loaded = true;
            bmp = nullptr;
            idx = (int)images_.size();
            images_.push_back(std::move(entry));
        }
    }
    if (bmp) delete bmp;
    return idx;
}

// Open the GDI+ bitmap for `idx` if it hasn't been opened yet, and read
// any animation metadata. Cheap no-op once already loaded. Called from
// EnsureImageRenderedAt_Public on first paint of each image.
void Renderer::LoadImageNow(int idx) {
    if (idx < 0 || idx >= (int)images_.size()) return;
    ImageEntry& e = images_[idx];
    if (e.loaded || e.loadFailed) return;
    EnsureGdiplus();
    Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromFile(e.path.c_str(), FALSE);
    if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok) {
        if (bmp) delete bmp;
        e.loadFailed = true;
        return;
    }
    // Detect time-based animation; record per-frame delays.
    UINT frameCount = 1;
    bool hasTimeDim = false;
    std::vector<unsigned int> delaysMs;
    UINT dimCount = bmp->GetFrameDimensionsCount();
    if (dimCount > 0) {
        std::vector<GUID> dims(dimCount);
        if (bmp->GetFrameDimensionsList(dims.data(), dimCount) == Gdiplus::Ok) {
            for (UINT d = 0; d < dimCount; ++d) {
                if (dims[d] == Gdiplus::FrameDimensionTime) {
                    UINT fc = bmp->GetFrameCount(&dims[d]);
                    if (fc > 1) {
                        frameCount = fc;
                        hasTimeDim = true;
                        UINT propSize = bmp->GetPropertyItemSize(0x5100);
                        if (propSize > 0) {
                            std::vector<BYTE> buf(propSize);
                            Gdiplus::PropertyItem* pi =
                                reinterpret_cast<Gdiplus::PropertyItem*>(buf.data());
                            if (bmp->GetPropertyItem(0x5100, propSize, pi) == Gdiplus::Ok
                                && pi->value && pi->length >= sizeof(UINT) * fc) {
                                const UINT* arr = (const UINT*)pi->value;
                                delaysMs.resize(fc);
                                for (UINT k = 0; k < fc; ++k) {
                                    unsigned int ms = arr[k] * 10;
                                    if (ms < 20) ms = 100;
                                    delaysMs[k] = ms;
                                }
                            }
                        }
                    }
                    break;
                }
            }
        }
    }
    if (delaysMs.empty()) delaysMs.assign(frameCount, 100);
    e.bmp         = bmp;
    e.totalFrames = (int)frameCount;
    e.hasTimeDim  = hasTimeDim;
    e.delaysMs    = std::move(delaysMs);
    e.loaded      = true;
}

int Renderer::GetImageWidth_Public(int idx) const {
    if (idx < 0 || idx >= (int)images_.size()) return 0;
    return images_[idx].width;
}
int Renderer::GetImageHeight_Public(int idx) const {
    if (idx < 0 || idx >= (int)images_.size()) return 0;
    return images_[idx].height;
}

void Renderer::PrefetchRemoteImages(int totalTimeoutMs) {
    auto isHttpUrl = [](const std::wstring& s) -> bool {
        if (s.size() < 7) return false;
        auto eqi = [](wchar_t a, wchar_t b) {
            return towlower((wint_t)a) == towlower((wint_t)b);
        };
        if (eqi(s[0], L'h') && eqi(s[1], L't') && eqi(s[2], L't') && eqi(s[3], L'p')) {
            if (s.size() >= 8 && eqi(s[4], L's')
                && s[5] == L':' && s[6] == L'/' && s[7] == L'/') return true;
            if (s[4] == L':' && s[5] == L'/' && s[6] == L'/') return true;
        }
        return false;
    };

    std::vector<std::wstring> urls;
    auto consider = [&](const std::wstring& s) {
        if (!isHttpUrl(s)) return;
        std::wstring cp = CachePathForUrl(s);
        if (cp.empty()) return;
        if (GetFileAttributesW(cp.c_str()) != INVALID_FILE_ATTRIBUTES) return;
        for (const auto& u : urls) if (u == s) return;     // de-dupe
        urls.push_back(s);
    };
    for (const Block& b : doc_.blocks) {
        if (b.type == BType::ImageBlock) consider(b.imagePath);
        for (const Inline& it : b.inlines) {
            if (it.kind == InlineKind::Image) consider(it.imagePath);
        }
    }
    if (urls.empty()) return;

    DWORD remaining   = (totalTimeoutMs > 0) ? (DWORD)totalTimeoutMs : 5000;
    DWORD chunkStart  = GetTickCount();
    size_t pos = 0;
    while (pos < urls.size()) {
        size_t end = pos + (size_t)MAXIMUM_WAIT_OBJECTS;
        if (end > urls.size()) end = urls.size();
        std::vector<HANDLE> handles;
        for (size_t i = pos; i < end; ++i) {
            DownloadCtx* ctx = new DownloadCtx;
            ctx->url  = urls[i];
            ctx->path = CachePathForUrl(urls[i]);
            HANDLE h = CreateThread(nullptr, 0, DownloadThreadProc, ctx, 0, nullptr);
            if (h) handles.push_back(h);
            else  delete ctx;
        }
        if (!handles.empty()) {
            DWORD elapsed = GetTickCount() - chunkStart;
            DWORD wait    = (elapsed >= remaining) ? 0 : (remaining - elapsed);
            WaitForMultipleObjects((DWORD)handles.size(),
                                    handles.data(), TRUE, wait);
            // Don't terminate threads still in flight — they'll write the
            // cache file at their own pace and the next session will use
            // it. Only close our reference handles.
            for (HANDLE h : handles) CloseHandle(h);
        }
        pos = end;
    }
}

void Renderer::EnsureImageRenderedAt_Public(int idx, int dispW, int dispH) {
    if (idx < 0 || idx >= (int)images_.size()) return;
    if (dispW < 1 || dispH < 1) return;
    ImageEntry& e = images_[idx];
    bool sizeOk  = (e.cachedHbm && e.cachedDispW == dispW && e.cachedDispH == dispH);
    bool frameOk = (e.cachedFrame == (int)e.currentFrame);
    if (sizeOk && frameOk) return;
    // Lazy decode: open the GDI+ Bitmap on first need so that ListLoad
    // can return as soon as Layout is finished — even for documents
    // with several big GIFs.
    if (!e.loaded && !e.loadFailed) LoadImageNow(idx);
    if (!e.bmp) return;
    if (e.hasTimeDim && e.totalFrames > 1) {
        GUID dim = Gdiplus::FrameDimensionTime;
        e.bmp->SelectActiveFrame(&dim, e.currentFrame);
    }
    if (e.cachedHbm) { DeleteObject(e.cachedHbm); e.cachedHbm = nullptr; }
    Gdiplus::Bitmap canvas((INT)dispW, (INT)dispH, PixelFormat32bppRGB);
    Gdiplus::Graphics g(&canvas);
    // Static images get the highest-quality resampler (re-render only on
    // resize/zoom, so the cost is paid once). Animated GIFs need to
    // re-render every tick — a faster bilinear is plenty there.
    if (e.totalFrames > 1) {
        g.SetInterpolationMode(Gdiplus::InterpolationModeBilinear);
    } else {
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    }
    g.SetSmoothingMode(Gdiplus::SmoothingModeNone);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    g.Clear(Gdiplus::Color(255, 255, 255, 255));
    g.DrawImage(e.bmp, 0, 0, (INT)dispW, (INT)dispH);
    HBITMAP hbm = nullptr;
    canvas.GetHBITMAP(Gdiplus::Color(255, 255, 255, 255), &hbm);
    e.cachedHbm   = hbm;
    e.cachedDispW = dispW;
    e.cachedDispH = dispH;
    e.cachedFrame = (int)e.currentFrame;
}

bool Renderer::HasAnimation() const {
    for (const auto& e : images_) if (e.totalFrames > 1) return true;
    return false;
}

bool Renderer::TickAnimation(unsigned int elapsedMs) {
    bool advanced = false;
    for (auto& e : images_) {
        if (e.totalFrames <= 1) continue;
        e.accumMs += elapsedMs;
        while (true) {
            unsigned int delay = (e.currentFrame < e.delaysMs.size())
                                     ? e.delaysMs[e.currentFrame] : 100;
            if (delay == 0) delay = 100;
            if (e.accumMs < delay) break;
            e.accumMs -= delay;
            e.currentFrame = (e.currentFrame + 1) % (unsigned int)e.totalFrames;
            advanced = true;
        }
    }
    return advanced;
}

// ---------- Layout helpers -------------------------------------------------

namespace {

int FontIndexForInline(const Inline& it) {
    bool bold = (it.style & S_BOLD)  != 0;
    bool ital = (it.style & S_ITALIC) != 0;
    if (it.style & S_CODE) {
        if (bold) return FK_CODE_B;
        if (ital) return FK_CODE_I;
        return FK_CODE;
    }
    if (bold && ital) return FK_BODY_BI;
    if (bold)         return FK_BODY_B;
    if (ital)         return FK_BODY_I;
    return FK_BODY;
}

int FontIndexForHeading(int level) {
    switch (level) {
        case 1: return FK_H1;
        case 2: return FK_H2;
        case 3: return FK_H3;
        case 4: return FK_H4;
        case 5: return FK_H5;
        default: return FK_H6;
    }
}

// True if a 32-bit Unicode code point is an emoji or pictograph that we
// should render via the Segoe UI Emoji font. Covers the common ranges:
//   0x2300-0x23FF, 0x2500-0x27BF, 0x2900-0x297F (misc + dingbats),
//   0x1F300-0x1FBFF (the main pictograph blocks).
// Variation selectors (FE0F) and ZWJ (200D) are treated as emoji-context
// so they ride along with the preceding glyph.
static bool IsEmojiCodepoint(uint32_t cp) {
    if (cp == 0xFE0F || cp == 0x200D) return true;
    if (cp >= 0x2300 && cp <= 0x23FF) return true;
    if (cp >= 0x2500 && cp <= 0x257F) return false;     // box-drawing — not emoji
    if (cp >= 0x2580 && cp <= 0x27BF) return true;
    if (cp >= 0x2900 && cp <= 0x297F) return true;
    if (cp >= 0x2B00 && cp <= 0x2BFF) return true;
    if (cp >= 0x3030 && cp <= 0x303D) return true;
    if (cp >= 0x1F000 && cp <= 0x1FBFF) return true;
    return false;
}

// Decode a UTF-16 character (or surrogate pair) at index i in s.
// Returns the codepoint and number of wchar_t consumed.
static uint32_t Utf16Decode(const std::wstring& s, size_t i, int* outAdvance) {
    if (i >= s.size()) { if (outAdvance) *outAdvance = 0; return 0; }
    wchar_t c = s[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size()) {
        wchar_t lo = s[i + 1];
        if (lo >= 0xDC00 && lo <= 0xDFFF) {
            uint32_t cp = 0x10000 + (((uint32_t)(c - 0xD800) << 10) | (lo - 0xDC00));
            if (outAdvance) *outAdvance = 2;
            return cp;
        }
    }
    if (outAdvance) *outAdvance = 1;
    return (uint32_t)c;
}

// True if the string contains at least one emoji-class codepoint.
[[maybe_unused]] static bool ContainsEmoji(const std::wstring& s) {
    size_t i = 0;
    while (i < s.size()) {
        int adv = 0;
        uint32_t cp = Utf16Decode(s, i, &adv);
        if (IsEmojiCodepoint(cp)) return true;
        if (adv <= 0) break;
        i += adv;
    }
    return false;
}

int MeasureRunWidth(Gdiplus::Graphics& g, Gdiplus::Font* font, const std::wstring& text) {
    if (text.empty() || !font) return 0;
    // GenericTypographic + MeasureTrailingSpaces -> tight, accurate widths
    // including any spaces at the end of the run.
    Gdiplus::StringFormat fmt(Gdiplus::StringFormat::GenericTypographic());
    fmt.SetFormatFlags(fmt.GetFormatFlags()
                       | Gdiplus::StringFormatFlagsMeasureTrailingSpaces
                       | Gdiplus::StringFormatFlagsNoWrap
                       | Gdiplus::StringFormatFlagsNoFitBlackBox);
    Gdiplus::RectF layout(0, 0, 100000.0f, 100000.0f);
    Gdiplus::RectF bound;
    g.MeasureString(text.c_str(), (INT)text.size(), font, layout, &fmt, &bound);
    // Round up to avoid clipping by 1px on rendered output.
    return (int)(bound.Width + 0.5f);
}

void DrawTextRun(Gdiplus::Graphics& g, Gdiplus::Font* font,
                 const std::wstring& text, int x, int y, COLORREF color) {
    if (!font || text.empty()) return;
    Gdiplus::SolidBrush brush(Gdiplus::Color(255,
        GetRValue(color), GetGValue(color), GetBValue(color)));
    Gdiplus::StringFormat fmt(Gdiplus::StringFormat::GenericTypographic());
    fmt.SetFormatFlags(fmt.GetFormatFlags()
                       | Gdiplus::StringFormatFlagsMeasureTrailingSpaces
                       | Gdiplus::StringFormatFlagsNoWrap
                       | Gdiplus::StringFormatFlagsNoFitBlackBox);
    g.DrawString(text.c_str(), (INT)text.size(), font,
                 Gdiplus::PointF((float)x, (float)y), &fmt, &brush);
}

// "Atom" used in line-breaking: either a word, a single space, an inline
// image, or a hard line break.
struct Atom {
    enum Kind { WORD, SPACE, IMAGE, BREAK } kind = WORD;
    std::wstring text;
    uint32_t style = 0;
    COLORREF color = C_TEXT;
    int      font  = FK_BODY;
    bool     underline = false;
    bool     strike = false;
    std::wstring linkUrl;
    int      imageIndex = -1;
    int      width  = 0;       // pixels
    int      height = 0;       // pixels (for images)
    int      ascent = 0;
};

// Tokenize a list of inlines into Atoms. Splits text on whitespace.
void TokenizeInlines(HDC hdc, Renderer& renderer,
                     const std::vector<Inline>& inlines,
                     std::vector<Atom>& out, COLORREF baseColor,
                     int defaultFont = FK_BODY,
                     bool inHeading = false) {
    (void)renderer;
    for (const auto& it : inlines) {
        if (it.kind == InlineKind::LineBreak) {
            Atom a; a.kind = Atom::BREAK; out.push_back(a);
            continue;
        }
        if (it.kind == InlineKind::Image) {
            Atom a;
            a.kind = Atom::IMAGE;
            a.imageIndex = -1;        // resolved at layout time
            a.text = it.imagePath;    // store path for the later Layout pass
            a.linkUrl = it.text;      // alt text (overload field)
            a.color = baseColor;
            a.font  = defaultFont;
            // Stash request-size in style + ascent fields. Style is a uint
            // bitfield, but we have plenty of unused bits here. Use width
            // and height fields directly via a piggyback: width=requested,
            // height=requested, and ascent=flag (1=W%, 2=H%). When width is
            // 0, no request -> use natural.
            a.width  = it.imageReqW;
            a.height = it.imageReqH;
            a.ascent = (it.imageWPct ? 1 : 0) | (it.imageHPct ? 2 : 0);
            out.push_back(a);
            continue;
        }
        // Text
        int fontIdx = inHeading ? defaultFont : FontIndexForInline(it);
        bool ulink = (it.style & S_LINK) != 0;
        bool strike = (it.style & S_STRIKE) != 0;
        COLORREF col = ulink ? C_LINK : baseColor;

        // Walk text splitting on whitespace runs.
        const std::wstring& s = it.text;
        size_t i = 0;
        while (i < s.size()) {
            // Split on whitespace
            if (s[i] == L' ' || s[i] == L'\t') {
                Atom a; a.kind = Atom::SPACE;
                a.text = L" ";
                a.font = fontIdx;
                a.color = col;
                a.style = it.style;
                a.linkUrl = it.linkUrl;
                a.underline = ulink;
                a.strike = strike;
                out.push_back(a);
                while (i < s.size() && (s[i] == L' ' || s[i] == L'\t')) ++i;
                continue;
            }
            // Word: read until whitespace, splitting at emoji/non-emoji
            // boundaries so the emoji segment can be drawn with Segoe UI
            // Emoji while the rest stays in the regular font.
            size_t wordStart = i;
            size_t segStart = i;
            bool   segIsEmoji = false;
            // Determine kind of first codepoint
            {
                int adv = 0;
                uint32_t cp = Utf16Decode(s, i, &adv);
                segIsEmoji = IsEmojiCodepoint(cp);
            }
            auto flushSeg = [&](size_t segEnd, bool emoji) {
                if (segEnd <= segStart) return;
                Atom a; a.kind = Atom::WORD;
                a.text = s.substr(segStart, segEnd - segStart);
                a.font = emoji ? FK_EMOJI : fontIdx;
                a.color = col;
                a.style = it.style;
                a.linkUrl = it.linkUrl;
                a.underline = ulink;
                a.strike = strike;
                out.push_back(a);
            };
            while (i < s.size() && s[i] != L' ' && s[i] != L'\t') {
                int adv = 0;
                uint32_t cp = Utf16Decode(s, i, &adv);
                bool isEmoji = IsEmojiCodepoint(cp);
                if (isEmoji != segIsEmoji) {
                    flushSeg(i, segIsEmoji);
                    segStart = i;
                    segIsEmoji = isEmoji;
                }
                i += (adv > 0 ? adv : 1);
            }
            flushSeg(i, segIsEmoji);
            (void)wordStart;
        }
    }
}

// Measure pending widths (using GDI+).
void MeasureAtoms(Gdiplus::Graphics& g, Renderer& r, std::vector<Atom>& atoms,
                  int innerWidth) {
    for (auto& a : atoms) {
        if (a.kind == Atom::IMAGE) {
            // resolve image
            if (!a.text.empty()) {
                int idx = r.LoadImageFromFile_Public(a.text);
                a.imageIndex = idx;
                if (idx >= 0) {
                    int natW = r.GetImageWidth_Public(idx);
                    int natH = r.GetImageHeight_Public(idx);
                    int reqW = a.width;
                    int reqH = a.height;
                    bool wPct = (a.ascent & 1) != 0;
                    bool hPct = (a.ascent & 2) != 0;
                    float zoom = r.GetZoom();
                    int targetW, targetH;
                    if (reqW > 0) {
                        if (wPct)
                            targetW = (int)(innerWidth * reqW * zoom / 100.0f + 0.5f);
                        else
                            targetW = (int)(reqW * zoom + 0.5f);
                        if (natW > 0) targetH = (int)((double)natH * targetW / natW + 0.5);
                        else          targetH = natH;
                    } else {
                        targetW = (int)(natW * zoom + 0.5f);
                        targetH = (int)(natH * zoom + 0.5f);
                        if (zoom <= 1.0f && targetW > innerWidth) {
                            if (targetW > 0)
                                targetH = (int)((double)targetH * innerWidth / targetW + 0.5);
                            targetW = innerWidth;
                        }
                    }
                    if (reqH > 0 && reqW <= 0) {
                        if (!hPct) targetH = (int)(reqH * zoom + 0.5f);
                        if (natH > 0) targetW = (int)((double)natW * targetH / natH + 0.5);
                    }
                    if (targetW < 1) targetW = 1;
                    if (targetH < 1) targetH = 1;
                    // Note: no eager render here — Paint will invoke
                    // EnsureImageRenderedAt_Public when this image is
                    // about to be drawn for the first time.
                    a.width  = targetW;
                    a.height = targetH;
                    a.ascent = 0;
                } else {
                    // Render alt text with brackets
                    a.kind = Atom::WORD;
                    std::wstring alt = a.linkUrl;
                    a.text = L"[" + (alt.empty() ? std::wstring(L"image") : alt) + L"]";
                    a.linkUrl.clear();
                    a.font = FK_BODY_I;
                    a.width = MeasureRunWidth(g, r.GetGdiplusFont_Public(a.font), a.text);
                    a.ascent = r.GetAscent_Public(a.font);
                    a.height = r.GetLineHeight_Public(a.font);
                }
            }
            continue;
        }
        a.width  = MeasureRunWidth(g, r.GetGdiplusFont_Public(a.font), a.text);
        a.ascent = r.GetAscent_Public(a.font);
        a.height = r.GetLineHeight_Public(a.font);
    }
}

} // namespace

// Renderer accessors used by the file-local helpers above.
int  Renderer::LoadImageFromFile_Public(const std::wstring& p) { return LoadImageFromFile(p); }

// ---------- Main layout ----------------------------------------------------

void Renderer::Layout(HWND hwnd, int contentWidth) {
    contentWidth_ = contentWidth;
    ops_.clear();
    DestroyImages();
    hits_.clear();
    anchors_.clear();
    currentHit_ = -1;

    if (!gpFonts_[FK_BODY]) CreateFonts(hwnd);

    // GDI+ measurement context shared with the helpers.
    HDC hdc = GetDC(hwnd);
    Gdiplus::Graphics graphics(hdc);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

    const int leftPad   = 20;
    const int rightPad  = 20;
    const int topPad    = 16;
    const int bottomPad = 24;

    int xOrigin = leftPad;
    int yCursor = topPad;
    int innerW  = contentWidth - leftPad - rightPad;
    if (innerW < 80) innerW = 80;

    auto emitLine = [&](std::vector<Atom>& line, int leftIndent, int innerWidth, bool justifyLast = false) {
        (void)justifyLast;
        if (line.empty()) return;
        // Trim trailing spaces
        while (!line.empty() && line.back().kind == Atom::SPACE) line.pop_back();
        if (line.empty()) return;
        // Compute line height (max ascent + max descent)
        int maxAsc = 0, maxDesc = 0, maxImgH = 0;
        for (const Atom& a : line) {
            if (a.kind == Atom::IMAGE) {
                maxImgH = std::max(maxImgH, a.height);
            } else {
                maxAsc = std::max(maxAsc, ascent_[a.font]);
                maxDesc = std::max(maxDesc, descent_[a.font] + (lineHeight_[a.font] - ascent_[a.font] - descent_[a.font]));
            }
        }
        int textLineH = maxAsc + maxDesc;
        int lineH = std::max(textLineH, maxImgH + 4);
        if (lineH < 16) lineH = 16;
        int baseline = yCursor + maxAsc;
        if (textLineH == 0 && maxImgH > 0) baseline = yCursor + maxImgH;

        int x = xOrigin + leftIndent;
        for (const Atom& a : line) {
            if (a.kind == Atom::IMAGE) {
                if (a.imageIndex >= 0) {
                    DrawOp op;
                    op.kind = D_IMAGE;
                    op.imageIndex = a.imageIndex;
                    op.x = x;
                    op.y = yCursor + (lineH - a.height) / 2;
                    op.w = a.width;
                    op.h = a.height;
                    ops_.push_back(op);
                }
                x += a.width;
                continue;
            }
            if (a.text.empty()) { x += a.width; continue; }
            DrawOp op;
            op.kind = D_TEXT;
            op.text = a.text;
            op.x = x;
            op.y = baseline - ascent_[a.font];
            op.w = a.width;
            op.h = lineHeight_[a.font];
            op.font = a.font;
            op.color = a.color;
            op.bg = (a.style & S_CODE) ? C_CODE_BG : NO_BG;
            op.underline = a.underline;
            op.strike = a.strike;
            op.linkUrl = a.linkUrl;
            op.runId = (int)ops_.size();
            ops_.push_back(op);
            x += a.width;
        }
        yCursor += lineH;
        line.clear();
    };

    auto wrapAtoms = [&](std::vector<Atom>& atoms, int leftIndent, int innerWidth) {
        std::vector<Atom> line;
        int curW = 0;
        bool firstLine = true;
        (void)firstLine;
        for (size_t i = 0; i < atoms.size(); ++i) {
            Atom& a = atoms[i];
            if (a.kind == Atom::BREAK) {
                emitLine(line, leftIndent, innerWidth);
                curW = 0;
                continue;
            }
            // Skip leading spaces on a line
            if (a.kind == Atom::SPACE && line.empty()) continue;
            // Long-word breaking: if a single word is wider than innerWidth,
            // we let it overflow rather than break mid-word. Common rendering
            // libraries do this for long URLs; cleaner than mid-word splits.
            int needed = a.width;
            if (curW + needed > innerWidth && !line.empty()) {
                emitLine(line, leftIndent, innerWidth);
                curW = 0;
                if (a.kind == Atom::SPACE) continue;
            }
            line.push_back(a);
            curW += needed;
        }
        emitLine(line, leftIndent, innerWidth);
    };

    auto vSpace = [&](int dy) { yCursor += dy; };

    // Per-block layout
    for (const Block& b : doc_.blocks) {
        switch (b.type) {
            case BType::Heading: {
                int fk = FontIndexForHeading(b.headingLevel);
                std::vector<Atom> atoms;
                TokenizeInlines(hdc, *this, b.inlines, atoms, C_TEXT, fk, /*inHeading=*/true);
                MeasureAtoms(graphics, *this, atoms, innerW);
                int top = yCursor;
                int saveX = xOrigin;
                int saveW = innerW;
                vSpace(b.headingLevel <= 2 ? 14 : 10);
                // Record an anchor for this heading using GitHub-style id
                // normalisation (lowercase, alnum + dash, spaces -> dash).
                {
                    std::wstring rawText;
                    for (const Inline& it : b.inlines) {
                        if (it.kind == InlineKind::Text) rawText += it.text;
                    }
                    std::wstring id;
                    bool prevDash = false;
                    for (wchar_t c : rawText) {
                        if (iswalnum((wint_t)c)) {
                            id.push_back((wchar_t)towlower((wint_t)c));
                            prevDash = false;
                        } else if (c == L' ' || c == L'-' || c == L'_') {
                            if (!prevDash && !id.empty()) {
                                id.push_back(L'-');
                                prevDash = true;
                            }
                        }
                    }
                    while (!id.empty() && id.back() == L'-') id.pop_back();
                    if (!id.empty()) {
                        AnchorEntry a;
                        a.id = id;
                        a.y  = yCursor;
                        anchors_.push_back(a);
                    }
                }
                wrapAtoms(atoms, 0, innerW);
                xOrigin = saveX; innerW = saveW;
                if (b.headingLevel == 1 || b.headingLevel == 2) {
                    // Bottom border
                    DrawOp op;
                    op.kind = D_RULE;
                    op.x = xOrigin;
                    op.y = yCursor + 4;
                    op.w = innerW;
                    op.h = 1;
                    op.color = C_BORDER;
                    ops_.push_back(op);
                    yCursor += 6;
                }
                vSpace(8);
                (void)top;
                break;
            }

            case BType::Paragraph: {
                std::vector<Atom> atoms;
                TokenizeInlines(hdc, *this, b.inlines, atoms, C_TEXT);
                MeasureAtoms(graphics, *this, atoms, innerW);
                wrapAtoms(atoms, 0, innerW);
                vSpace(8);
                break;
            }

            case BType::CodeBlock: {
                int fk = FK_CODE;
                int lh = lineHeight_[fk];
                // Split code into lines
                std::vector<std::wstring> lines;
                std::wstring cur;
                for (wchar_t c : b.codeText) {
                    if (c == L'\n') { lines.push_back(std::move(cur)); cur.clear(); }
                    else cur.push_back(c);
                }
                lines.push_back(std::move(cur));
                while (!lines.empty() && lines.back().empty()) lines.pop_back();

                int blockTop = yCursor;
                int padX = 12, padY = 10;
                int blockH = (int)lines.size() * lh + padY * 2;
                if (lines.empty()) blockH = lh + padY * 2;

                // Background
                DrawOp bg;
                bg.kind = D_RECT;
                bg.x = xOrigin;
                bg.y = blockTop;
                bg.w = innerW;
                bg.h = blockH;
                bg.color = C_BLOCK_BG;
                ops_.push_back(bg);

                // Border
                DrawOp brd;
                brd.kind = D_LINE;
                brd.x = xOrigin; brd.y = blockTop;
                brd.w = innerW; brd.h = blockH;
                brd.color = C_BORDER;
                brd.penWidth = 1;
                ops_.push_back(brd);

                // Lines
                int lineY = blockTop + padY;
                for (auto& ln : lines) {
                    DrawOp op;
                    op.kind = D_TEXT;
                    op.font = fk;
                    op.color = C_TEXT;
                    op.bg = NO_BG;
                    op.text = ln;
                    op.x = xOrigin + padX;
                    op.y = lineY;
                    op.w = MeasureRunWidth(graphics, gpFonts_[fk], ln);
                    op.h = lh;
                    op.runId = (int)ops_.size();
                    ops_.push_back(op);
                    lineY += lh;
                }
                yCursor = blockTop + blockH + 8;
                break;
            }

            case BType::BlockQuote: {
                std::vector<Atom> atoms;
                TokenizeInlines(hdc, *this, b.inlines, atoms, C_MUTED);
                MeasureAtoms(graphics, *this, atoms, innerW);
                int saveX = xOrigin, saveW = innerW;
                int barX = xOrigin;
                int indent = 12;
                xOrigin += indent;
                innerW  -= indent;
                int top = yCursor;
                vSpace(2);
                wrapAtoms(atoms, 0, innerW);
                int bot = yCursor;

                // Left bar
                DrawOp bar;
                bar.kind = D_RECT;
                bar.x = barX;
                bar.y = top;
                bar.w = 4;
                bar.h = bot - top;
                bar.color = C_BORDER;
                ops_.push_back(bar);

                xOrigin = saveX; innerW = saveW;
                vSpace(8);
                break;
            }

            case BType::HorizontalRule: {
                vSpace(6);
                DrawOp r;
                r.kind = D_RULE;
                r.x = xOrigin;
                r.y = yCursor;
                r.w = innerW;
                r.h = 2;
                r.color = C_RULE;
                ops_.push_back(r);
                vSpace(12);
                break;
            }

            case BType::UnorderedList:
            case BType::OrderedList: {
                bool ord = (b.type == BType::OrderedList);
                int  num = b.listStartIndex;
                int  bulletAreaW = ord ? std::max(28, avgChar_[FK_BODY] * 4) : 22;
                for (size_t li = 0; li < b.listItems.size(); ++li) {
                    const ListItem& it = b.listItems[li];
                    int itemTop = yCursor;
                    // Bullet / number
                    if (it.taskMarker) {
                        DrawOp cb;
                        cb.kind = D_CHECKBOX;
                        cb.x = xOrigin + 4;
                        cb.y = yCursor + 3;
                        cb.w = 14;
                        cb.h = 14;
                        cb.color = C_TEXT;
                        cb.checked = it.taskChecked;
                        ops_.push_back(cb);
                    } else if (ord) {
                        wchar_t buf[16];
                        wsprintfW(buf, L"%d.", num + (int)li);
                        int w = MeasureRunWidth(graphics, gpFonts_[FK_BODY], buf);
                        DrawOp t;
                        t.kind = D_TEXT;
                        t.text = buf;
                        t.font = FK_BODY;
                        t.color = C_TEXT;
                        t.bg = NO_BG;
                        t.x = xOrigin + bulletAreaW - w - 6;
                        t.y = yCursor;
                        t.w = w;
                        t.h = lineHeight_[FK_BODY];
                        ops_.push_back(t);
                    } else {
                        // bullet glyph
                        DrawOp t;
                        t.kind = D_TEXT;
                        t.text = L"\u2022";
                        t.font = FK_BODY_B;
                        t.color = C_TEXT;
                        t.bg = NO_BG;
                        int w = MeasureRunWidth(graphics, gpFonts_[FK_BODY_B], t.text);
                        t.x = xOrigin + 8;
                        t.y = yCursor;
                        t.w = w;
                        t.h = lineHeight_[FK_BODY_B];
                        ops_.push_back(t);
                    }
                    // Content
                    int saveX = xOrigin, saveW = innerW;
                    xOrigin += bulletAreaW;
                    innerW  -= bulletAreaW;
                    std::vector<Atom> atoms;
                    TokenizeInlines(hdc, *this, it.inlines, atoms, C_TEXT);
                    MeasureAtoms(graphics, *this, atoms, innerW);
                    wrapAtoms(atoms, 0, innerW);
                    xOrigin = saveX; innerW = saveW;
                    vSpace(2);
                    (void)itemTop;
                }
                vSpace(6);
                break;
            }

            case BType::Table: {
                if (b.tableHeader.cells.empty()) break;
                int ncol = (int)b.tableHeader.cells.size();
                if (ncol == 0) break;

                // Compute column widths from longest cell text (no wrapping
                // inside cells in v1; long cells will widen the column).
                std::vector<int> colW(ncol, 0);
                auto cellText = [](const TableCell& c) {
                    std::wstring s;
                    for (const auto& it : c.inlines) {
                        if (it.kind == InlineKind::Text) s += it.text;
                        else if (it.kind == InlineKind::Image) s += L"[img]";
                    }
                    return s;
                };
                int padX = 10, padY = 6;
                for (int c = 0; c < ncol; ++c) {
                    int w = MeasureRunWidth(graphics, gpFonts_[FK_BODY_B], cellText(b.tableHeader.cells[c]));
                    colW[c] = std::max(colW[c], w);
                }
                for (const auto& row : b.tableBody) {
                    for (int c = 0; c < ncol && c < (int)row.cells.size(); ++c) {
                        int w = MeasureRunWidth(graphics, gpFonts_[FK_BODY], cellText(row.cells[c]));
                        colW[c] = std::max(colW[c], w);
                    }
                }
                int totalW = 0;
                for (int w : colW) totalW += w + padX * 2;
                // Distribute extra width if narrower than innerW
                if (totalW < innerW) {
                    int extra = (innerW - totalW) / ncol;
                    for (auto& w : colW) w += extra;
                    totalW = 0;
                    for (int w : colW) totalW += w + padX * 2;
                }
                int rowH = lineHeight_[FK_BODY_B] + padY * 2;
                int tableTop = yCursor;
                int x0 = xOrigin;
                int curY = yCursor;

                // Header background
                DrawOp hbg;
                hbg.kind = D_RECT;
                hbg.x = x0; hbg.y = curY;
                hbg.w = totalW; hbg.h = rowH;
                hbg.color = C_BLOCK_BG;
                ops_.push_back(hbg);

                // Header cells
                {
                    int cx = x0;
                    for (int c = 0; c < ncol; ++c) {
                        const TableCell& cell = b.tableHeader.cells[c];
                        std::wstring t = cellText(cell);
                        int tw = MeasureRunWidth(graphics, gpFonts_[FK_BODY_B], t);
                        int align = (c < (int)b.tableAligns.size()) ? b.tableAligns[c] : ALIGN_LEFT;
                        int cellW = colW[c] + padX * 2;
                        int tx = cx + padX;
                        if (align == ALIGN_CENTER) tx = cx + (cellW - tw) / 2;
                        else if (align == ALIGN_RIGHT) tx = cx + cellW - tw - padX;
                        DrawOp op;
                        op.kind = D_TEXT;
                        op.font = FK_BODY_B;
                        op.color = C_TEXT;
                        op.bg = NO_BG;
                        op.text = t;
                        op.x = tx;
                        op.y = curY + padY;
                        op.w = tw;
                        op.h = lineHeight_[FK_BODY_B];
                        ops_.push_back(op);
                        cx += cellW;
                    }
                }
                curY += rowH;

                int bodyRowH = lineHeight_[FK_BODY] + padY * 2;
                for (size_t ri = 0; ri < b.tableBody.size(); ++ri) {
                    const auto& row = b.tableBody[ri];
                    if (ri % 2 == 1) {
                        DrawOp rbg;
                        rbg.kind = D_RECT;
                        rbg.x = x0; rbg.y = curY;
                        rbg.w = totalW; rbg.h = bodyRowH;
                        rbg.color = RGB(250, 250, 252);
                        ops_.push_back(rbg);
                    }
                    int cx = x0;
                    for (int c = 0; c < ncol; ++c) {
                        if (c >= (int)row.cells.size()) { cx += colW[c] + padX * 2; continue; }
                        const TableCell& cell = row.cells[c];
                        std::wstring t = cellText(cell);
                        int tw = MeasureRunWidth(graphics, gpFonts_[FK_BODY], t);
                        int align = (c < (int)b.tableAligns.size()) ? b.tableAligns[c] : ALIGN_LEFT;
                        int cellW = colW[c] + padX * 2;
                        int tx = cx + padX;
                        if (align == ALIGN_CENTER) tx = cx + (cellW - tw) / 2;
                        else if (align == ALIGN_RIGHT) tx = cx + cellW - tw - padX;
                        DrawOp op;
                        op.kind = D_TEXT;
                        op.font = FK_BODY;
                        op.color = C_TEXT;
                        op.bg = NO_BG;
                        op.text = t;
                        op.x = tx;
                        op.y = curY + padY;
                        op.w = tw;
                        op.h = lineHeight_[FK_BODY];
                        ops_.push_back(op);
                        cx += cellW;
                    }
                    curY += bodyRowH;
                }

                // Outline + grid lines
                DrawOp box;
                box.kind = D_LINE;
                box.x = x0; box.y = tableTop;
                box.w = totalW; box.h = curY - tableTop;
                box.color = C_BORDER;
                box.penWidth = 1;
                ops_.push_back(box);
                // Row separators
                {
                    int yy = tableTop + rowH;
                    DrawOp sep;
                    sep.kind = D_RULE;
                    sep.x = x0; sep.y = yy;
                    sep.w = totalW; sep.h = 1;
                    sep.color = C_BORDER;
                    ops_.push_back(sep);
                    for (size_t ri = 0; ri < b.tableBody.size(); ++ri) {
                        yy += bodyRowH;
                        DrawOp s2 = sep;
                        s2.y = yy;
                        ops_.push_back(s2);
                    }
                }
                // Column separators
                {
                    int cx = x0;
                    for (int c = 0; c < ncol - 1; ++c) {
                        cx += colW[c] + padX * 2;
                        DrawOp v;
                        v.kind = D_RECT;
                        v.x = cx; v.y = tableTop;
                        v.w = 1; v.h = curY - tableTop;
                        v.color = C_BORDER;
                        ops_.push_back(v);
                    }
                }
                yCursor = curY + 12;
                break;
            }

            case BType::ImageBlock: {
                int idx = LoadImageFromFile(b.imagePath);
                if (idx >= 0) {
                    int natW = GetImageWidth_Public(idx);
                    int natH = GetImageHeight_Public(idx);
                    int targetW = natW;
                    int targetH = natH;
                    // Apply zoom uniformly to every kind of width spec so
                    // the user sees images grow/shrink with Ctrl+wheel.
                    if (b.imageReqW > 0) {
                        if (b.imageWPct)
                            targetW = (int)(innerW * b.imageReqW * zoom_ / 100.0f + 0.5f);
                        else
                            targetW = (int)(b.imageReqW * zoom_ + 0.5f);
                        if (natW > 0) targetH = (int)((double)natH * targetW / natW + 0.5);
                    } else {
                        // Natural size scaled by zoom, capped to innerW only
                        // when zoom <= 1 so larger zoom levels remain visible
                        // even if the image is wider than the viewport.
                        targetW = (int)(natW * zoom_ + 0.5f);
                        targetH = (int)(natH * zoom_ + 0.5f);
                        if (zoom_ <= 1.0f && targetW > innerW) {
                            if (targetW > 0)
                                targetH = (int)((double)targetH * innerW / targetW + 0.5);
                            targetW = innerW;
                        }
                    }
                    if (b.imageReqH > 0 && b.imageReqW <= 0) {
                        if (!b.imageHPct) targetH = (int)(b.imageReqH * zoom_ + 0.5f);
                        if (natH > 0) targetW = (int)((double)natW * targetH / natH + 0.5);
                    }
                    if (targetW < 1) targetW = 1;
                    if (targetH < 1) targetH = 1;
                    // Centre percent-width images that ended up narrower
                    // than the content area. Browsers don't show this
                    // problem because Ctrl+- shrinks the entire viewport
                    // proportionally; we shrink only the image, so
                    // left-aligning a 50%-wide image leaves a giant gap
                    // on the right. Centring removes that asymmetry.
                    // For overflowing (zoom > 1) images we keep the
                    // left-align so the user can at least see the left
                    // edge of the picture.
                    int drawX = xOrigin;
                    if (b.imageWPct && targetW < innerW) {
                        drawX = xOrigin + (innerW - targetW) / 2;
                    }
                    DrawOp op;
                    op.kind = D_IMAGE;
                    op.imageIndex = idx;
                    op.x = drawX;
                    op.y = yCursor;
                    op.w = targetW;
                    op.h = targetH;
                    ops_.push_back(op);
                    yCursor += targetH + 8;
                } else {
                    // Render as italic alt text
                    std::wstring t = L"[image: " + (b.imageAlt.empty() ? b.imagePath : b.imageAlt) + L"]";
                    int w = MeasureRunWidth(graphics, gpFonts_[FK_BODY_I], t);
                    DrawOp op;
                    op.kind = D_TEXT;
                    op.font = FK_BODY_I;
                    op.color = C_MUTED;
                    op.bg = NO_BG;
                    op.text = t;
                    op.x = xOrigin;
                    op.y = yCursor;
                    op.w = w;
                    op.h = lineHeight_[FK_BODY_I];
                    ops_.push_back(op);
                    yCursor += lineHeight_[FK_BODY_I] + 4;
                }
                break;
            }

            case BType::Empty:
            default:
                yCursor += 6;
                break;
        }
    }

    ReleaseDC(hwnd, hdc);

    totalHeight_ = yCursor + bottomPad;

    // Re-compute search hits if there was an active query.
    if (!lastQuery_.empty()) RecomputeHits();
}

// ---------- Painting -------------------------------------------------------

void Renderer::Paint(HDC hdc, int scrollY, int clientW, int clientH) {
    // Background
    RECT cr = { 0, 0, clientW, clientH };
    HBRUSH bg = CreateSolidBrush(C_BG);
    FillRect(hdc, &cr, bg);
    DeleteObject(bg);

    int yMin = scrollY;
    int yMax = scrollY + clientH;

    // GDI+ Graphics over the buffer DC for text rendering. Using
    // AntiAliasGridFit gives nice anti-aliased text on any DPI and
    // permits color emoji rendering through GDI+'s DirectWrite path on
    // Windows 10+ (Segoe UI Emoji is the system fallback).
    Gdiplus::Graphics graphics(hdc);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);

    SetBkMode(hdc, TRANSPARENT);

    // Helper: rectangle from doc coords
    auto docToClient = [&](int x, int y) -> POINT { POINT p = { x, y - scrollY }; return p; };

    // Pass 1: rectangles, rules, lines, images (under text)
    for (size_t i = 0; i < ops_.size(); ++i) {
        const DrawOp& op = ops_[i];
        if (op.y + op.h < yMin) continue;
        if (op.y > yMax) break;
        if (op.kind == D_RECT) {
            HBRUSH br = CreateSolidBrush(op.color);
            POINT p = docToClient(op.x, op.y);
            RECT  r = { p.x, p.y, p.x + op.w, p.y + op.h };
            FillRect(hdc, &r, br);
            DeleteObject(br);
        } else if (op.kind == D_RULE) {
            HBRUSH br = CreateSolidBrush(op.color);
            POINT p = docToClient(op.x, op.y);
            RECT  r = { p.x, p.y, p.x + op.w, p.y + std::max(1, op.h) };
            FillRect(hdc, &r, br);
            DeleteObject(br);
        } else if (op.kind == D_LINE) {
            // Draw a hollow rectangle (outline)
            HPEN pen = CreatePen(PS_SOLID, std::max(1, op.penWidth), op.color);
            HPEN oldPen = (HPEN)SelectObject(hdc, pen);
            HBRUSH oldBr = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
            POINT p = docToClient(op.x, op.y);
            Rectangle(hdc, p.x, p.y, p.x + op.w, p.y + op.h);
            SelectObject(hdc, oldPen);
            SelectObject(hdc, oldBr);
            DeleteObject(pen);
        } else if (op.kind == D_IMAGE) {
            if (op.imageIndex < 0 || op.imageIndex >= (int)images_.size()) continue;
            // Cache must be valid at op.w x op.h (Layout calls
            // EnsureImageRenderedAt_Public for that). For animated GIFs
            // a frame-tick may have invalidated the cache — re-render in
            // place at the *display* size so we never StretchBlt at paint.
            EnsureImageRenderedAt_Public(op.imageIndex, op.w, op.h);
            ImageEntry& ent = images_[op.imageIndex];
            HBITMAP hb = ent.cachedHbm;
            if (!hb) continue;
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP oldBmp = (HBITMAP)SelectObject(mem, hb);
            POINT p = docToClient(op.x, op.y);
            // Plain BitBlt — no scaling — because the cache already
            // matches op.w x op.h. This is the speed-up.
            BitBlt(hdc, p.x, p.y, op.w, op.h, mem, 0, 0, SRCCOPY);
            SelectObject(mem, oldBmp);
            DeleteDC(mem);
        } else if (op.kind == D_CHECKBOX) {
            POINT p = docToClient(op.x, op.y);
            RECT r = { p.x, p.y, p.x + op.w, p.y + op.h };
            HBRUSH bg2 = CreateSolidBrush(op.checked ? RGB(8, 80, 200) : RGB(255, 255, 255));
            FillRect(hdc, &r, bg2);
            DeleteObject(bg2);
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(140, 150, 160));
            HPEN oldPen = (HPEN)SelectObject(hdc, pen);
            HBRUSH oldBr = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, r.left, r.top, r.right, r.bottom);
            SelectObject(hdc, oldPen);
            SelectObject(hdc, oldBr);
            DeleteObject(pen);
            if (op.checked) {
                // Check mark
                pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
                oldPen = (HPEN)SelectObject(hdc, pen);
                MoveToEx(hdc, r.left + 3, r.top + 7, nullptr);
                LineTo(hdc, r.left + 6, r.top + 10);
                LineTo(hdc, r.right - 3, r.top + 4);
                SelectObject(hdc, oldPen);
                DeleteObject(pen);
            }
        }
    }

    // Pass 2: search-hit highlight rectangles UNDER text
    if (!hits_.empty()) {
        for (const auto& h : hits_) {
            const DrawOp& op = ops_[h.opIndex];
            if (op.kind != D_TEXT) continue;
            if (op.y + op.h < yMin) continue;
            if (op.y > yMax) continue;

            // Measure substring positions via GDI+
            int xPre = 0, xMatchEnd = 0;
            if (h.charStart > 0) {
                xPre = MeasureRunWidth(graphics, gpFonts_[op.font],
                                       op.text.substr(0, h.charStart));
            }
            int upto = h.charStart + h.charLen;
            if (upto > (int)op.text.size()) upto = (int)op.text.size();
            xMatchEnd = MeasureRunWidth(graphics, gpFonts_[op.font],
                                        op.text.substr(0, upto));

            POINT p = docToClient(op.x + xPre, op.y);
            RECT r = { p.x, p.y, p.x + (xMatchEnd - xPre), p.y + op.h };
            bool isCur = (h.globalIndex == currentHit_);
            HBRUSH br = CreateSolidBrush(isCur ? C_HL_CUR : C_HL);
            FillRect(hdc, &r, br);
            DeleteObject(br);
            if (isCur) {
                HPEN pen = CreatePen(PS_SOLID, 1, C_HL_CUR_BORDER);
                HPEN oldPen = (HPEN)SelectObject(hdc, pen);
                HBRUSH oldBr = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
                Rectangle(hdc, r.left, r.top, r.right, r.bottom);
                SelectObject(hdc, oldPen);
                SelectObject(hdc, oldBr);
                DeleteObject(pen);
            }
        }
    }

    // Pass 2b: selection highlight (under text, over backgrounds).
    if (HasSelectionAtPaint()) {
        for (int i = selA_op_; i <= selB_op_ && i < (int)ops_.size(); ++i) {
            if (ops_[i].kind != D_TEXT) continue;
            const DrawOp& op = ops_[i];
            if (op.y + op.h < yMin) continue;
            if (op.y > yMax) break;
            int from = (i == selA_op_) ? selA_char_ : 0;
            int to   = (i == selB_op_) ? selB_char_ : (int)op.text.size();
            if (from < 0) from = 0;
            if (to > (int)op.text.size()) to = (int)op.text.size();
            if (to <= from) continue;
            int xPre = (from > 0)
                ? MeasureRunWidth(graphics, gpFonts_[op.font], op.text.substr(0, from))
                : 0;
            int xEnd = MeasureRunWidth(graphics, gpFonts_[op.font], op.text.substr(0, to));
            POINT p = docToClient(op.x + xPre, op.y);
            RECT r = { p.x, p.y, p.x + (xEnd - xPre), p.y + op.h };
            HBRUSH br = CreateSolidBrush(RGB(173, 204, 255));
            FillRect(hdc, &r, br);
            DeleteObject(br);
        }
    }

    // Pass 3: text + inline-code backgrounds + underline + strike
    // Drawn via GDI+ DrawString — supports anti-aliased text. Emoji-font
    // runs are skipped here and drawn with Direct2D + DirectWrite below
    // so their COLR/CPAL color glyphs come out in full color.
    for (const DrawOp& op : ops_) {
        if (op.kind != D_TEXT) continue;
        if (op.font == FK_EMOJI) continue;     // handled by D2D pass
        if (op.y + op.h < yMin) continue;
        if (op.y > yMax) break;

        if (op.bg != NO_BG) {
            HBRUSH br = CreateSolidBrush(op.bg);
            POINT p = docToClient(op.x - 2, op.y);
            RECT r = { p.x, p.y, p.x + op.w + 4, p.y + op.h };
            FillRect(hdc, &r, br);
            DeleteObject(br);
        }

        POINT p = docToClient(op.x, op.y);
        DrawTextRun(graphics, gpFonts_[op.font], op.text, p.x, p.y, op.color);

        if (op.underline) {
            int y = p.y + ascent_[op.font] + 1;
            HPEN pen = CreatePen(PS_SOLID, 1, op.color);
            HPEN oldPen = (HPEN)SelectObject(hdc, pen);
            MoveToEx(hdc, p.x, y, nullptr);
            LineTo(hdc, p.x + op.w, y);
            SelectObject(hdc, oldPen);
            DeleteObject(pen);
        }
        if (op.strike) {
            int y = p.y + ascent_[op.font] / 2 + 2;
            HPEN pen = CreatePen(PS_SOLID, 1, op.color);
            HPEN oldPen = (HPEN)SelectObject(hdc, pen);
            MoveToEx(hdc, p.x, y, nullptr);
            LineTo(hdc, p.x + op.w, y);
            SelectObject(hdc, oldPen);
            DeleteObject(pen);
        }
    }

    // GDI+ may have buffered output; flush via destruction-time semantics.
    // Some sources recommend explicit Flush before mixing with D2D.
    graphics.Flush(Gdiplus::FlushIntentionSync);

    // Pass 3b: emoji runs through Direct2D / DirectWrite, which honour
    // COLR/CPAL color font tables (e.g. Segoe UI Emoji on Windows 10+).
    if (dwEmojiFormat_ && g_d2dFactory) {
        D2D1_RENDER_TARGET_PROPERTIES rtProps =
            D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
                96.0f, 96.0f);
        ID2D1DCRenderTarget* rt = nullptr;
        if (g_d2dFactory->CreateDCRenderTarget(&rtProps, &rt) == S_OK && rt) {
            RECT bind = { 0, 0, clientW, clientH };
            if (rt->BindDC(hdc, &bind) == S_OK) {
                rt->BeginDraw();
                ID2D1SolidColorBrush* brush = nullptr;
                rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &brush);

                IDWriteTextFormat* fmt = (IDWriteTextFormat*)dwEmojiFormat_;
                for (const DrawOp& op : ops_) {
                    if (op.kind != D_TEXT) continue;
                    if (op.font != FK_EMOJI) continue;
                    if (op.y + op.h < yMin) continue;
                    if (op.y > yMax) break;
                    POINT p = docToClient(op.x, op.y);
                    D2D1_RECT_F layout = D2D1::RectF(
                        (float)p.x, (float)p.y,
                        (float)(p.x + op.w + 200),  // generous width to avoid clipping
                        (float)(p.y + op.h + 8));
                    if (brush) {
                        rt->DrawText(op.text.c_str(), (UINT32)op.text.size(),
                                     fmt, &layout, brush,
                                     D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT,
                                     DWRITE_MEASURING_MODE_NATURAL);
                    }
                }
                if (brush) brush->Release();
                rt->EndDraw();
            }
            rt->Release();
        }
    }
}

// ---------- Search ---------------------------------------------------------

static std::wstring ToLower(const std::wstring& s) {
    std::wstring r = s;
    for (auto& c : r) c = (wchar_t)towlower(c);
    return r;
}

void Renderer::ClearSearch() {
    hits_.clear();
    currentHit_ = -1;
    lastQuery_.clear();
}

int Renderer::ApplySearch(const std::wstring& q, bool caseSensitive) {
    lastQuery_ = q;
    lastCaseSens_ = caseSensitive;
    RecomputeHits();
    if (!hits_.empty()) currentHit_ = 0;
    else                currentHit_ = -1;
    return (int)hits_.size();
}

void Renderer::RecomputeHits() {
    hits_.clear();
    if (lastQuery_.empty()) return;
    std::wstring needle = lastCaseSens_ ? lastQuery_ : ToLower(lastQuery_);
    int gi = 0;
    for (size_t i = 0; i < ops_.size(); ++i) {
        if (ops_[i].kind != D_TEXT) continue;
        std::wstring h = lastCaseSens_ ? ops_[i].text : ToLower(ops_[i].text);
        size_t pos = 0;
        while (true) {
            size_t f = h.find(needle, pos);
            if (f == std::wstring::npos) break;
            SearchHit sh;
            sh.opIndex = (int)i;
            sh.charStart = (int)f;
            sh.charLen = (int)needle.size();
            sh.globalIndex = gi++;
            hits_.push_back(sh);
            pos = f + needle.size();
        }
    }
}

void Renderer::SetCurrentMatch(int idx) {
    if (hits_.empty()) { currentHit_ = -1; return; }
    if (idx < 0) idx = (int)hits_.size() - 1;
    if (idx >= (int)hits_.size()) idx = 0;
    currentHit_ = idx;
}

bool Renderer::GetMatchYRange(int idx, int* outY, int* outH) const {
    if (idx < 0 || idx >= (int)hits_.size()) return false;
    const SearchHit& h = hits_[idx];
    const DrawOp& op = ops_[h.opIndex];
    if (outY) *outY = op.y;
    if (outH) *outH = op.h;
    return true;
}

std::wstring Renderer::GetPlainText() const {
    std::wstring r;
    int lastY = INT_MIN;
    for (const auto& op : ops_) {
        if (op.kind != D_TEXT) continue;
        if (op.y != lastY && lastY != INT_MIN) r.push_back(L'\n');
        lastY = op.y;
        r += op.text;
    }
    if (!r.empty() && r.back() != L'\n') r.push_back(L'\n');
    return r;
}

std::wstring Renderer::HitTestLink(int x, int yDoc) const {
    for (const auto& op : ops_) {
        if (op.kind != D_TEXT) continue;
        if (op.linkUrl.empty()) continue;
        if (yDoc < op.y || yDoc >= op.y + op.h) continue;
        if (x < op.x || x >= op.x + op.w) continue;
        return op.linkUrl;
    }
    return L"";
}

bool Renderer::FindAnchor(const std::wstring& anchorId, int* outY) const {
    // Normalise the incoming id the same way we do at heading-record time.
    std::wstring needle;
    bool prevDash = false;
    for (wchar_t c : anchorId) {
        if (iswalnum((wint_t)c)) {
            needle.push_back((wchar_t)towlower((wint_t)c));
            prevDash = false;
        } else if (c == L' ' || c == L'-' || c == L'_' || c == L'%' || c == L'+') {
            if (!prevDash && !needle.empty()) {
                needle.push_back(L'-');
                prevDash = true;
            }
        }
    }
    while (!needle.empty() && needle.back() == L'-') needle.pop_back();
    if (needle.empty()) return false;
    for (const auto& a : anchors_) {
        if (a.id == needle) {
            if (outY) *outY = a.y;
            return true;
        }
    }
    return false;
}

// ---------- Selection ------------------------------------------------------

bool Renderer::HitTestText(int x, int yDoc, int* outOpIndex, int* outCharOffset) const {
    // First, find an op whose Y range contains yDoc. Pick the closest in X.
    int bestIdx = -1;
    int bestDx  = INT_MAX;
    int bestY   = INT_MAX;
    for (size_t i = 0; i < ops_.size(); ++i) {
        const DrawOp& op = ops_[i];
        if (op.kind != D_TEXT) continue;
        if (yDoc < op.y || yDoc >= op.y + op.h) continue;
        int dx = 0;
        if (x < op.x)             dx = op.x - x;
        else if (x > op.x + op.w) dx = x - (op.x + op.w);
        if (dx < bestDx || (dx == bestDx && op.y < bestY)) {
            bestDx = dx;
            bestIdx = (int)i;
            bestY = op.y;
        }
    }
    if (bestIdx < 0) {
        // Find closest by Y instead — clamp to nearest line.
        int bestVDist = INT_MAX;
        for (size_t i = 0; i < ops_.size(); ++i) {
            const DrawOp& op = ops_[i];
            if (op.kind != D_TEXT) continue;
            int vdist = 0;
            if (yDoc < op.y) vdist = op.y - yDoc;
            else if (yDoc > op.y + op.h) vdist = yDoc - (op.y + op.h);
            if (vdist < bestVDist) {
                bestVDist = vdist;
                bestIdx = (int)i;
            }
        }
        if (bestIdx < 0) return false;
    }
    const DrawOp& op = ops_[bestIdx];
    // Find char offset by binary searching widths
    int relX = x - op.x;
    if (relX <= 0) {
        if (outOpIndex) *outOpIndex = bestIdx;
        if (outCharOffset) *outCharOffset = 0;
        return true;
    }
    if (relX >= op.w) {
        if (outOpIndex) *outOpIndex = bestIdx;
        if (outCharOffset) *outCharOffset = (int)op.text.size();
        return true;
    }
    // Linear scan through text -- typical lines are short.
    HDC hdc = GetDC(nullptr);
    Gdiplus::Graphics g(hdc);
    int lo = 0, hi = (int)op.text.size();
    while (lo + 1 < hi) {
        int mid = (lo + hi) / 2;
        int w = MeasureRunWidth(g, gpFonts_[op.font], op.text.substr(0, mid));
        if (w <= relX) lo = mid;
        else           hi = mid;
    }
    // Choose between lo and hi based on which is closer
    int wLo = MeasureRunWidth(g, gpFonts_[op.font], op.text.substr(0, lo));
    int wHi = MeasureRunWidth(g, gpFonts_[op.font], op.text.substr(0, hi));
    int chosen = (relX - wLo < wHi - relX) ? lo : hi;
    ReleaseDC(nullptr, hdc);
    if (outOpIndex) *outOpIndex = bestIdx;
    if (outCharOffset) *outCharOffset = chosen;
    return true;
}

// Helper: compare two (op, char) positions in document order.
static inline bool PosLess(int aOp, int aCh, int bOp, int bCh) {
    if (aOp != bOp) return aOp < bOp;
    return aCh < bCh;
}

void Renderer::SetSelection(int aOp, int aChar, int bOp, int bChar) {
    if (aOp < 0 || bOp < 0) {
        ClearSelection();
        return;
    }
    if (PosLess(bOp, bChar, aOp, aChar)) {
        std::swap(aOp, bOp);
        std::swap(aChar, bChar);
    }
    selA_op_ = aOp; selA_char_ = aChar;
    selB_op_ = bOp; selB_char_ = bChar;
}

void Renderer::ClearSelection() {
    selA_op_ = selB_op_ = -1;
    selA_char_ = selB_char_ = 0;
}

bool Renderer::HasSelection() const {
    return selA_op_ >= 0 && selB_op_ >= 0
        && !(selA_op_ == selB_op_ && selA_char_ == selB_char_);
}

std::wstring Renderer::GetSelectionText() const {
    if (!HasSelection()) return L"";
    std::wstring out;
    int lastY = INT_MIN;
    for (int i = selA_op_; i <= selB_op_ && i < (int)ops_.size(); ++i) {
        if (ops_[i].kind != D_TEXT) continue;
        const DrawOp& op = ops_[i];
        // Insert newline when Y advances to a new line.
        if (op.y != lastY && lastY != INT_MIN) out.push_back(L'\n');
        lastY = op.y;
        int from = (i == selA_op_) ? selA_char_ : 0;
        int to   = (i == selB_op_) ? selB_char_ : (int)op.text.size();
        if (from < 0) from = 0;
        if (to > (int)op.text.size()) to = (int)op.text.size();
        if (to > from) out += op.text.substr(from, to - from);
    }
    return out;
}

} // namespace md
