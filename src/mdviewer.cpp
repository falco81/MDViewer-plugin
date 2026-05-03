// mdviewer.cpp -- Total Commander Lister plugin: native Markdown viewer.
//
// No HTML, no Internet Explorer. Pure Win32 + GDI/GDI+:
//   * One owner-drawn window per opened .md file.
//   * Vertical scrollbar (real, working, with arrows / wheel / drag /
//     keyboard navigation).
//   * Search overlay: child Edit + Prev/Next/Close buttons in the top-right
//     corner; live highlighting of matches; Enter = next, Shift+Enter = prev.
//   * Esc closes the search overlay; if the overlay is closed Esc closes
//     the viewer (forwards VK_ESCAPE to TC's Lister window).
//   * Ctrl+C copies the rendered plain text. Ctrl+A is a no-op (we have
//     no selection model in v1).
//
// Plugin entry points exported (see mdviewer.def):
//   ListGetDetectString, ListLoad, ListLoadW, ListCloseWindow,
//   ListSearchText, ListSearchTextW, ListSendCommand, ListSetDefaultParams.

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shlwapi.h>

#include <algorithm>
#include <string>
#include <vector>

#include "listplug.h"
#include "parser.h"
#include "renderer.h"

// ---------------------------------------------------------------------------
// Per-instance plugin state
// ---------------------------------------------------------------------------
struct PluginInst {
    HWND parentList = nullptr;     // TC's Lister parent
    HWND hwnd       = nullptr;     // our viewer window (child of parentList)
    HWND searchBar  = nullptr;     // search overlay container (child of hwnd)
    HWND searchEdit = nullptr;     // edit control inside search bar
    HWND searchInfo = nullptr;     // static "n / m" label
    HWND btnPrev    = nullptr;
    HWND btnNext    = nullptr;
    HWND btnClose   = nullptr;
    bool searchOpen = false;

    // Zoom indicator widget (top-right, only visible when zoom != 100%).
    HWND zoomBar      = nullptr;
    HWND zoomLabel    = nullptr;
    HWND zoomBtnReset = nullptr;

    md::Renderer renderer;
    md::Document doc;

    int  scrollY      = 0;
    int  contentH     = 0;        // total laid-out height
    int  clientW      = 0;
    int  clientH      = 0;
    int  lineStep     = 24;       // rough scroll-by-line distance
    float zoomLevel   = 1.0f;
    bool placementApplied = false;

    // Selection: anchor + caret in (opIndex, charOffset) form. When
    // anchor == caret, selection is empty. selecting = true means a
    // mouse-drag is in progress.
    bool selecting = false;
    int  selAnchorOp   = -1;
    int  selAnchorChar = 0;
    int  selCaretOp    = -1;
    int  selCaretChar  = 0;

    std::wstring filePath;
};

static const wchar_t* kClassName = L"MDViewerNativeWindow";
static const wchar_t* kSearchBarClass = L"MDViewerSearchBar";
static const wchar_t* kZoomBarClass   = L"MDViewerZoomBar";

static HMODULE g_hInstance = nullptr;

// IDs for child controls
enum {
    ID_EDIT  = 1001,
    ID_PREV  = 1002,
    ID_NEXT  = 1003,
    ID_CLOSE = 1004,
    ID_ZOOM_RESET = 1005,
};

// ---------------------------------------------------------------------------
// Forward decls
// ---------------------------------------------------------------------------
static LRESULT CALLBACK ViewerWndProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK SearchBarProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK ZoomBarProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK EditSubclassProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

static void OpenSearch(PluginInst* p);
static void CloseSearch(PluginInst* p);
static void RunSearch(PluginInst* p, bool focusFirst);
static void GotoMatch(PluginInst* p, int idx);
static void NextMatch(PluginInst* p);
static void PrevMatch(PluginInst* p);
static void UpdateSearchLabel(PluginInst* p);

static void ScrollBy(PluginInst* p, int dy);
static void ScrollTo(PluginInst* p, int y);
static void UpdateScrollbar(PluginInst* p);
static void RelayoutAll(PluginInst* p);
static void ApplyZoom(PluginInst* p, float newZoom);
static void ExportToPdf(PluginInst* p);
static void UpdateZoomWidget(PluginInst* p);
static void PositionZoomWidget(PluginInst* p);

// INI persistence (window placement)
static std::wstring GetIniPath();
static void LoadWindowPlacement(HWND hwnd);
static void SaveWindowPlacement(HWND hwnd);

// ---------------------------------------------------------------------------
// INI persistence (window placement next to the DLL)
// ---------------------------------------------------------------------------
static std::wstring GetIniPath() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(g_hInstance, path, MAX_PATH);
    std::wstring s = path;
    size_t a = s.find_last_of(L"\\/");
    if (a != std::wstring::npos) s = s.substr(0, a + 1);
    s += L"MDViewer.ini";
    return s;
}

static void LoadWindowPlacement(HWND hwnd) {
    if (!hwnd) return;
    std::wstring ini = GetIniPath();
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) return;

    int x = GetPrivateProfileIntW(L"Window", L"X",      INT_MIN, ini.c_str());
    int y = GetPrivateProfileIntW(L"Window", L"Y",      INT_MIN, ini.c_str());
    int w = GetPrivateProfileIntW(L"Window", L"W",      0, ini.c_str());
    int h = GetPrivateProfileIntW(L"Window", L"H",      0, ini.c_str());
    int m = GetPrivateProfileIntW(L"Window", L"Maximized", 0, ini.c_str());
    if (x == INT_MIN || y == INT_MIN || w <= 0 || h <= 0) return;

    // Sanity-check against the virtual screen so we don't restore off-screen.
    int vsX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vsY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vsW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vsH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    RECT vr = { vsX, vsY, vsX + vsW, vsY + vsH };
    RECT wr = { x, y, x + w, y + h };
    RECT inter;
    if (!IntersectRect(&inter, &wr, &vr)) return;

    // Use SetWindowPlacement so that BOTH the normal-position rectangle
    // AND the show-state (maximized vs normal) get applied. SetWindowPos
    // alone can't un-maximize a window TC opened maximized by default.
    WINDOWPLACEMENT wp = {};
    wp.length = sizeof(wp);
    GetWindowPlacement(hwnd, &wp);
    wp.rcNormalPosition.left   = x;
    wp.rcNormalPosition.top    = y;
    wp.rcNormalPosition.right  = x + w;
    wp.rcNormalPosition.bottom = y + h;
    wp.showCmd = m ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    wp.flags   = 0;
    SetWindowPlacement(hwnd, &wp);
}

static void SaveWindowPlacement(HWND hwnd) {
    if (!hwnd) return;
    WINDOWPLACEMENT wp = {};
    wp.length = sizeof(wp);
    if (!GetWindowPlacement(hwnd, &wp)) return;
    RECT r = wp.rcNormalPosition;
    int  x = r.left, y = r.top;
    int  w = r.right - r.left;
    int  h = r.bottom - r.top;
    int  m = (wp.showCmd == SW_SHOWMAXIMIZED) ? 1 : 0;

    std::wstring ini = GetIniPath();
    wchar_t buf[32];
    wsprintfW(buf, L"%d", x); WritePrivateProfileStringW(L"Window", L"X", buf, ini.c_str());
    wsprintfW(buf, L"%d", y); WritePrivateProfileStringW(L"Window", L"Y", buf, ini.c_str());
    wsprintfW(buf, L"%d", w); WritePrivateProfileStringW(L"Window", L"W", buf, ini.c_str());
    wsprintfW(buf, L"%d", h); WritePrivateProfileStringW(L"Window", L"H", buf, ini.c_str());
    wsprintfW(buf, L"%d", m); WritePrivateProfileStringW(L"Window", L"Maximized", buf, ini.c_str());
}

// ---------------------------------------------------------------------------
// Zoom
// ---------------------------------------------------------------------------
static void ApplyZoom(PluginInst* p, float newZoom) {
    if (newZoom < 0.5f) newZoom = 0.5f;
    if (newZoom > 3.0f) newZoom = 3.0f;
    if (std::abs(newZoom - p->zoomLevel) < 0.001f) return;
    // Try to keep current scroll proportional
    float ratio = (p->contentH > 0) ? (float)p->scrollY / (float)p->contentH : 0.0f;
    p->zoomLevel = newZoom;
    p->renderer.SetZoom(newZoom);
    RelayoutAll(p);
    p->scrollY = (int)(ratio * p->contentH + 0.5f);
    if (p->scrollY > p->contentH - p->clientH)
        p->scrollY = std::max(0, p->contentH - p->clientH);
    UpdateScrollbar(p);
    UpdateZoomWidget(p);
    InvalidateRect(p->hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// PDF export via the standard print dialog. Users select "Microsoft Print
// to PDF" (default on Windows 10+) and pick a filename in the resulting
// save dialog. We re-layout the document at the printer's DPI / page width
// and emit one printer page per virtual viewport.
// ---------------------------------------------------------------------------
static void ExportToPdf(PluginInst* p) {
    PRINTDLGW pd = {};
    pd.lStructSize = sizeof(pd);
    pd.hwndOwner   = p->parentList ? p->parentList : p->hwnd;
    pd.Flags       = PD_RETURNDC | PD_NOPAGENUMS | PD_NOSELECTION
                   | PD_USEDEVMODECOPIESANDCOLLATE;
    if (!PrintDlgW(&pd) || !pd.hDC) return;

    HDC printDC = pd.hDC;
    int pageW   = GetDeviceCaps(printDC, HORZRES);
    int pageH   = GetDeviceCaps(printDC, VERTRES);
    int marginX = GetDeviceCaps(printDC, LOGPIXELSX) / 2;   // 0.5 inch
    int marginY = GetDeviceCaps(printDC, LOGPIXELSY) / 2;
    int contentW = pageW - 2 * marginX;
    int contentH = pageH - 2 * marginY;

    // Build a fresh renderer over the printer DC so fonts get sized at the
    // printer's DPI. We move the document into it (and back) since
    // Renderer::SetDocument moves the value.
    md::Renderer printRenderer;
    md::Document docCopy = p->doc;        // doc_ inside p->renderer was moved
                                           // earlier; we cached p->doc so we
                                           // have a copy to work from.
    printRenderer.SetDocument(std::move(docCopy));

    // Layout into a hidden child of p->hwnd whose DC is the printer DC...
    // Actually, Renderer::Layout takes an HWND for GetDC; we need to give
    // it the printer DC instead. Add a tiny wrapper: use p->hwnd to
    // initialize fonts at screen DPI then re-layout at print DPI through
    // the printer DC. Easiest: call Layout once with p->hwnd as a stand-in
    // (not ideal but adequate for v1 export).
    printRenderer.Layout(p->hwnd, contentW);
    int total = printRenderer.GetTotalHeight();

    DOCINFOW di = {};
    di.cbSize = sizeof(di);
    di.lpszDocName = L"MDViewer Document";
    if (StartDocW(printDC, &di) <= 0) {
        DeleteDC(printDC);
        return;
    }

    int yCursor = 0;
    int pageIdx = 0;
    while (yCursor < total) {
        if (StartPage(printDC) <= 0) break;
        // Save / restore world transform so consecutive pages start at the
        // same physical origin. We translate the printer DC by (marginX,
        // marginY - yCursor) to draw the slice [yCursor, yCursor+contentH).
        SetGraphicsMode(printDC, GM_ADVANCED);
        XFORM xf;
        xf.eM11 = 1; xf.eM12 = 0;
        xf.eM21 = 0; xf.eM22 = 1;
        xf.eDx  = (float)marginX;
        xf.eDy  = (float)marginY;
        SetWorldTransform(printDC, &xf);

        printRenderer.Paint(printDC, yCursor, contentW, contentH);

        // Reset transform
        ModifyWorldTransform(printDC, nullptr, MWT_IDENTITY);
        SetGraphicsMode(printDC, GM_COMPATIBLE);
        EndPage(printDC);

        yCursor += contentH;
        ++pageIdx;
        if (pageIdx > 500) break;   // safety bound
    }
    EndDoc(printDC);
    DeleteDC(printDC);
}

// ---------------------------------------------------------------------------
// Zoom indicator widget (top-right; visible while zoom != 100%)
// ---------------------------------------------------------------------------
static LRESULT CALLBACK ZoomBarProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PluginInst* p = (PluginInst*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_CREATE: {
            CREATESTRUCT* cs = (CREATESTRUCT*)lp;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            return 0;
        }
        case WM_COMMAND: {
            if (!p) break;
            int id = LOWORD(wp);
            if (id == ID_ZOOM_RESET) {
                ApplyZoom(p, 1.0f);
                return 0;
            }
            break;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void EnsureZoomWidget(PluginInst* p) {
    if (p->zoomBar) return;
    p->zoomBar = CreateWindowExW(WS_EX_NOPARENTNOTIFY,
        kZoomBarClass, L"",
        WS_CHILD | WS_BORDER,
        0, 0, 1, 1, p->hwnd, nullptr, g_hInstance, p);
    p->zoomLabel = CreateWindowExW(0,
        L"STATIC", L"100%",
        WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
        0, 0, 10, 10, p->zoomBar, nullptr, g_hInstance, nullptr);
    p->zoomBtnReset = CreateWindowExW(0,
        L"BUTTON", L"Reset",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 10, 10, p->zoomBar, (HMENU)(INT_PTR)ID_ZOOM_RESET, g_hInstance, nullptr);
    HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    SendMessage(p->zoomLabel,    WM_SETFONT, (WPARAM)f, TRUE);
    SendMessage(p->zoomBtnReset, WM_SETFONT, (WPARAM)f, TRUE);
}

static void PositionZoomWidget(PluginInst* p) {
    if (!p->zoomBar) return;
    const int W = 140, H = 32, MARGIN = 8;
    int sbW = GetSystemMetrics(SM_CXVSCROLL);
    int x = p->clientW - sbW - W - MARGIN;
    int y = MARGIN;
    // If search bar is currently shown, push the zoom widget below it.
    if (p->searchOpen) y += 36 + MARGIN;
    if (x < MARGIN) x = MARGIN;
    SetWindowPos(p->zoomBar, HWND_TOP, x, y, W, H, SWP_NOACTIVATE);

    int pad = 4;
    int btnW = 50;
    int labelW = W - btnW - pad * 3;
    SetWindowPos(p->zoomLabel,    nullptr, pad,                  pad, labelW, H - pad * 2, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(p->zoomBtnReset, nullptr, pad + labelW + pad,   pad, btnW,   H - pad * 2, SWP_NOZORDER | SWP_NOACTIVATE);
}

static void UpdateZoomWidget(PluginInst* p) {
    bool needShow = std::abs(p->zoomLevel - 1.0f) > 0.001f;
    if (!needShow) {
        if (p->zoomBar) ShowWindow(p->zoomBar, SW_HIDE);
        return;
    }
    EnsureZoomWidget(p);
    PositionZoomWidget(p);
    wchar_t buf[32];
    wsprintfW(buf, L"%d%%", (int)(p->zoomLevel * 100.0f + 0.5f));
    SetWindowTextW(p->zoomLabel, buf);
    ShowWindow(p->zoomBar, SW_SHOW);
}

// ---------------------------------------------------------------------------
// File loading
// ---------------------------------------------------------------------------
static std::vector<unsigned char> ReadFileBinary(const std::wstring& path, size_t maxBytes) {
    std::vector<unsigned char> data;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return data;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0) { CloseHandle(h); return data; }
    size_t n = (size_t)std::min<long long>(sz.QuadPart, (long long)maxBytes);
    data.resize(n);
    DWORD got = 0;
    if (!ReadFile(h, data.data(), (DWORD)n, &got, nullptr)) data.clear();
    else data.resize(got);
    CloseHandle(h);
    return data;
}

static std::wstring ParentDir(const std::wstring& path) {
    size_t a = path.find_last_of(L"\\/");
    if (a == std::wstring::npos) return L"";
    return path.substr(0, a);
}

// ---------------------------------------------------------------------------
// Window class registration
// ---------------------------------------------------------------------------
static void RegisterClassesOnce() {
    static bool done = false;
    if (done) return;
    done = true;

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    {
        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = ViewerWndProc;
        wc.hInstance     = g_hInstance;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;     // we paint manually
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);
    }
    {
        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = SearchBarProc;
        wc.hInstance     = g_hInstance;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kSearchBarClass;
        RegisterClassExW(&wc);
    }
    {
        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = ZoomBarProc;
        wc.hInstance     = g_hInstance;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kZoomBarClass;
        RegisterClassExW(&wc);
    }
}

// ---------------------------------------------------------------------------
// Layout / scroll helpers
// ---------------------------------------------------------------------------
static void RelayoutAll(PluginInst* p) {
    if (!p || !p->hwnd) return;
    RECT cr;
    GetClientRect(p->hwnd, &cr);
    p->clientW = cr.right - cr.left;
    p->clientH = cr.bottom - cr.top;
    int sbW = GetSystemMetrics(SM_CXVSCROLL);
    int contentW = p->clientW - sbW;
    if (contentW < 100) contentW = 100;
    p->renderer.Layout(p->hwnd, contentW);
    p->contentH = p->renderer.GetTotalHeight();
    if (p->scrollY > p->contentH - p->clientH)
        p->scrollY = std::max(0, p->contentH - p->clientH);
    UpdateScrollbar(p);
    InvalidateRect(p->hwnd, nullptr, FALSE);
}

static void UpdateScrollbar(PluginInst* p) {
    SCROLLINFO si = {};
    si.cbSize = sizeof(si);
    si.fMask  = SIF_ALL | SIF_DISABLENOSCROLL;
    si.nMin   = 0;
    si.nMax   = std::max(0, p->contentH - 1);
    si.nPage  = p->clientH;
    si.nPos   = p->scrollY;
    SetScrollInfo(p->hwnd, SB_VERT, &si, TRUE);
}

static void ScrollTo(PluginInst* p, int y) {
    int maxY = std::max(0, p->contentH - p->clientH);
    if (y < 0) y = 0;
    if (y > maxY) y = maxY;
    int dy = y - p->scrollY;
    if (dy == 0) return;
    p->scrollY = y;
    SetScrollPos(p->hwnd, SB_VERT, p->scrollY, TRUE);
    RECT cr;
    GetClientRect(p->hwnd, &cr);
    ScrollWindowEx(p->hwnd, 0, -dy, nullptr, &cr, nullptr, nullptr,
                   SW_INVALIDATE | SW_ERASE);
    UpdateWindow(p->hwnd);
}

static void ScrollBy(PluginInst* p, int dy) {
    ScrollTo(p, p->scrollY + dy);
}

// ---------------------------------------------------------------------------
// Search bar UI
// ---------------------------------------------------------------------------
static void PositionSearchBar(PluginInst* p) {
    if (!p->searchBar) return;
    const int W = 360, H = 36, MARGIN = 8;
    int sbW = GetSystemMetrics(SM_CXVSCROLL);
    int x = p->clientW - sbW - W - MARGIN;
    int y = MARGIN;
    if (x < MARGIN) x = MARGIN;
    SetWindowPos(p->searchBar, HWND_TOP, x, y, W, H, SWP_NOACTIVATE);

    // Children
    int pad = 6;
    int btnW = 28, btnH = H - 2 * pad;
    int infoW = 60;
    int editW = W - infoW - 3 * btnW - 5 * pad;
    SetWindowPos(p->searchEdit, nullptr, pad, pad, editW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(p->searchInfo, nullptr, pad + editW + pad, pad + 2, infoW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);
    int xb = pad + editW + pad + infoW + pad;
    SetWindowPos(p->btnPrev,  nullptr, xb,                   pad, btnW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(p->btnNext,  nullptr, xb + btnW,            pad, btnW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(p->btnClose, nullptr, xb + 2 * btnW,        pad, btnW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);
}

static void UpdateSearchLabel(PluginInst* p) {
    if (!p->searchInfo) return;
    int total = p->renderer.GetMatchCount();
    int cur   = p->renderer.GetCurrentMatch();
    wchar_t buf[64];
    if (total <= 0) {
        wcscpy(buf, L"0 / 0");
    } else {
        wsprintfW(buf, L"%d / %d", cur + 1, total);
    }
    SetWindowTextW(p->searchInfo, buf);
}

static void OpenSearch(PluginInst* p) {
    if (!p->searchBar) {
        p->searchBar = CreateWindowExW(WS_EX_NOPARENTNOTIFY,
            kSearchBarClass, L"",
            WS_CHILD | WS_BORDER,
            0, 0, 1, 1, p->hwnd, nullptr, g_hInstance, p);
        p->searchEdit = CreateWindowExW(WS_EX_CLIENTEDGE,
            L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            0, 0, 10, 10, p->searchBar, (HMENU)(INT_PTR)ID_EDIT, g_hInstance, nullptr);
        p->searchInfo = CreateWindowExW(0,
            L"STATIC", L"0 / 0",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            0, 0, 10, 10, p->searchBar, nullptr, g_hInstance, nullptr);
        p->btnPrev = CreateWindowExW(0,
            L"BUTTON", L"\u25B2",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 10, 10, p->searchBar, (HMENU)(INT_PTR)ID_PREV, g_hInstance, nullptr);
        p->btnNext = CreateWindowExW(0,
            L"BUTTON", L"\u25BC",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 10, 10, p->searchBar, (HMENU)(INT_PTR)ID_NEXT, g_hInstance, nullptr);
        p->btnClose = CreateWindowExW(0,
            L"BUTTON", L"X",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 10, 10, p->searchBar, (HMENU)(INT_PTR)ID_CLOSE, g_hInstance, nullptr);

        // Use system UI font for child controls
        HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        SendMessage(p->searchEdit, WM_SETFONT, (WPARAM)f, TRUE);
        SendMessage(p->searchInfo, WM_SETFONT, (WPARAM)f, TRUE);
        SendMessage(p->btnPrev,    WM_SETFONT, (WPARAM)f, TRUE);
        SendMessage(p->btnNext,    WM_SETFONT, (WPARAM)f, TRUE);
        SendMessage(p->btnClose,   WM_SETFONT, (WPARAM)f, TRUE);

        // Subclass the edit control so we can capture Enter / Esc / arrows
        SetWindowSubclass(p->searchEdit, EditSubclassProc, 1, (DWORD_PTR)p);
    }
    p->searchOpen = true;
    PositionSearchBar(p);
    ShowWindow(p->searchBar, SW_SHOW);
    SetFocus(p->searchEdit);
    SendMessage(p->searchEdit, EM_SETSEL, 0, -1);
    UpdateSearchLabel(p);
    if (p->zoomBar && IsWindowVisible(p->zoomBar)) PositionZoomWidget(p);
}

static void CloseSearch(PluginInst* p) {
    if (!p->searchOpen) return;
    p->searchOpen = false;
    if (p->searchBar) ShowWindow(p->searchBar, SW_HIDE);
    p->renderer.ClearSearch();
    if (p->zoomBar && IsWindowVisible(p->zoomBar)) PositionZoomWidget(p);
    SetFocus(p->hwnd);
    InvalidateRect(p->hwnd, nullptr, FALSE);
}

static void RunSearch(PluginInst* p, bool focusFirst) {
    wchar_t buf[512];
    GetWindowTextW(p->searchEdit, buf, 512);
    std::wstring q = buf;
    if (q.empty()) {
        p->renderer.ClearSearch();
        UpdateSearchLabel(p);
        InvalidateRect(p->hwnd, nullptr, FALSE);
        return;
    }
    int n = p->renderer.ApplySearch(q, /*caseSensitive=*/false);
    if (n > 0 && focusFirst) GotoMatch(p, 0);
    else                     UpdateSearchLabel(p);
    InvalidateRect(p->hwnd, nullptr, FALSE);
}

static void GotoMatch(PluginInst* p, int idx) {
    if (p->renderer.GetMatchCount() <= 0) return;
    p->renderer.SetCurrentMatch(idx);
    int y = 0, h = 0;
    if (p->renderer.GetMatchYRange(p->renderer.GetCurrentMatch(), &y, &h)) {
        // Center match in viewport, clamped
        int target = y - p->clientH / 3;
        ScrollTo(p, target);
    }
    UpdateSearchLabel(p);
    InvalidateRect(p->hwnd, nullptr, FALSE);
}

static void NextMatch(PluginInst* p) {
    int n = p->renderer.GetMatchCount();
    if (n <= 0) return;
    int idx = p->renderer.GetCurrentMatch() + 1;
    if (idx >= n) idx = 0;
    GotoMatch(p, idx);
}
static void PrevMatch(PluginInst* p) {
    int n = p->renderer.GetMatchCount();
    if (n <= 0) return;
    int idx = p->renderer.GetCurrentMatch() - 1;
    if (idx < 0) idx = n - 1;
    GotoMatch(p, idx);
}

// ---------------------------------------------------------------------------
// Search-bar window procedure
// ---------------------------------------------------------------------------
static LRESULT CALLBACK SearchBarProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PluginInst* p = (PluginInst*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_CREATE: {
            CREATESTRUCT* cs = (CREATESTRUCT*)lp;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            return 0;
        }
        case WM_COMMAND: {
            if (!p) break;
            int id = LOWORD(wp);
            int code = HIWORD(wp);
            if (id == ID_EDIT && code == EN_CHANGE) {
                RunSearch(p, /*focusFirst=*/true);
                return 0;
            }
            if (id == ID_PREV)  { PrevMatch(p); SetFocus(p->searchEdit); return 0; }
            if (id == ID_NEXT)  { NextMatch(p); SetFocus(p->searchEdit); return 0; }
            if (id == ID_CLOSE) { CloseSearch(p); return 0; }
            break;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Subclass the edit control to handle special keys.
static LRESULT CALLBACK EditSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                          UINT_PTR /*idSubclass*/, DWORD_PTR refData) {
    PluginInst* p = (PluginInst*)refData;
    switch (msg) {
        case WM_KEYDOWN: {
            bool shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
            bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            (void)ctrl;
            if (wp == VK_RETURN) {
                if (shift) PrevMatch(p);
                else       NextMatch(p);
                return 0;
            }
            if (wp == VK_ESCAPE) {
                CloseSearch(p);
                return 0;
            }
            if (wp == VK_F3 || wp == VK_F7) {
                if (shift) PrevMatch(p);
                else       NextMatch(p);
                return 0;
            }
            break;
        }
        case WM_CHAR: {
            // Eat Enter / Esc characters so the edit control doesn't beep
            if (wp == VK_RETURN || wp == VK_ESCAPE) return 0;
            break;
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Clipboard helper
// ---------------------------------------------------------------------------
static void CopyToClipboard(HWND hwnd, const std::wstring& text) {
    if (!OpenClipboard(hwnd)) return;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hMem) {
        wchar_t* dst = (wchar_t*)GlobalLock(hMem);
        if (dst) {
            memcpy(dst, text.c_str(), bytes);
            GlobalUnlock(hMem);
            SetClipboardData(CF_UNICODETEXT, hMem);
        } else {
            GlobalFree(hMem);
        }
    }
    CloseClipboard();
}

// ---------------------------------------------------------------------------
// Main viewer window proc
// ---------------------------------------------------------------------------
static LRESULT CALLBACK ViewerWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PluginInst* p = (PluginInst*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
        case WM_CREATE: {
            CREATESTRUCT* cs = (CREATESTRUCT*)lp;
            p = (PluginInst*)cs->lpCreateParams;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)p);
            p->hwnd = hwnd;
            return 0;
        }

        case WM_SIZE: {
            if (!p) break;
            RelayoutAll(p);
            if (p->searchOpen) PositionSearchBar(p);
            if (p->zoomBar && IsWindowVisible(p->zoomBar)) PositionZoomWidget(p);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;       // we paint background ourselves

        case WM_PAINT: {
            if (!p) break;
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            // Double-buffer to avoid flicker
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, p->clientW, p->clientH);
            HBITMAP oldBmp = (HBITMAP)SelectObject(mem, bmp);
            p->renderer.Paint(mem, p->scrollY, p->clientW, p->clientH);
            BitBlt(hdc, 0, 0, p->clientW, p->clientH, mem, 0, 0, SRCCOPY);
            SelectObject(mem, oldBmp);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_VSCROLL: {
            if (!p) break;
            SCROLLINFO si = {}; si.cbSize = sizeof(si); si.fMask = SIF_ALL;
            GetScrollInfo(hwnd, SB_VERT, &si);
            int y = si.nPos;
            int code = LOWORD(wp);
            switch (code) {
                case SB_LINEUP:    y -= p->lineStep; break;
                case SB_LINEDOWN:  y += p->lineStep; break;
                case SB_PAGEUP:    y -= std::max(1, p->clientH - p->lineStep); break;
                case SB_PAGEDOWN:  y += std::max(1, p->clientH - p->lineStep); break;
                case SB_TOP:       y = 0; break;
                case SB_BOTTOM:    y = p->contentH; break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: y = si.nTrackPos; break;
            }
            ScrollTo(p, y);
            return 0;
        }

        case WM_MOUSEWHEEL: {
            if (!p) break;
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            bool shift = (LOWORD(wp) & MK_SHIFT) != 0;
            bool ctrl  = (LOWORD(wp) & MK_CONTROL) != 0;
            if (shift || ctrl) {
                // Shift+wheel or Ctrl+wheel = zoom
                float step = (delta > 0) ? 0.1f : -0.1f;
                ApplyZoom(p, p->zoomLevel + step);
                return 0;
            }
            UINT lines = 3;
            SystemParametersInfo(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
            if (lines == 0) lines = 3;
            int dy = -(delta * (int)lines * p->lineStep / WHEEL_DELTA);
            ScrollBy(p, dy);
            return 0;
        }

        case WM_KEYDOWN: {
            if (!p) break;
            bool shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
            bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            switch (wp) {
                case VK_UP:       ScrollBy(p, -p->lineStep); return 0;
                case VK_DOWN:     ScrollBy(p,  p->lineStep); return 0;
                case VK_PRIOR:    ScrollBy(p, -(p->clientH - p->lineStep)); return 0;
                case VK_NEXT:     ScrollBy(p,  (p->clientH - p->lineStep)); return 0;
                case VK_HOME:     ScrollTo(p, 0); return 0;
                case VK_END:      ScrollTo(p, p->contentH); return 0;
                case VK_SPACE:    ScrollBy(p,  (p->clientH - p->lineStep)); return 0;
                case VK_BACK:     ScrollBy(p, -(p->clientH - p->lineStep)); return 0;
                case VK_F7:
                case VK_F3:
                    if (p->renderer.GetMatchCount() > 0) {
                        if (shift) PrevMatch(p);
                        else       NextMatch(p);
                    } else {
                        OpenSearch(p);
                    }
                    return 0;
                case VK_ESCAPE:
                    if (p->searchOpen) { CloseSearch(p); return 0; }
                    // Forward Esc to TC's Lister so it closes the viewer.
                    if (p->parentList) PostMessage(p->parentList, WM_KEYDOWN, VK_ESCAPE, 0);
                    return 0;
                case 'F':
                    if (ctrl) { OpenSearch(p); return 0; }
                    break;
                case 'C':
                    if (ctrl) {
                        std::wstring txt = p->renderer.HasSelection()
                            ? p->renderer.GetSelectionText()
                            : p->renderer.GetPlainText();
                        CopyToClipboard(hwnd, txt);
                        return 0;
                    }
                    break;
                case 'A':
                    if (ctrl) {
                        // Select all: from (0,0) to last text op, last char.
                        std::wstring all = p->renderer.GetPlainText();
                        // Set sentinel selection that covers entire document.
                        // Use INT_MAX-1 as "last op" — the renderer clamps.
                        p->renderer.SetSelection(0, 0, INT_MAX - 1, 0);
                        // Better: walk to find a real last op.
                        // Workaround: do a trick - use a hit test at the very
                        // end-of-document Y to find the last op.
                        int lastOp = 0, lastChar = 0;
                        if (p->renderer.HitTestText(INT_MAX, p->contentH + 1000,
                                                     &lastOp, &lastChar)) {
                            p->renderer.SetSelection(0, 0, lastOp, INT_MAX);
                        }
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                    break;
                case 'E':
                    if (ctrl) {
                        ExportToPdf(p);
                        return 0;
                    }
                    break;
                case VK_ADD:
                case VK_OEM_PLUS:
                    if (ctrl) { ApplyZoom(p, p->zoomLevel + 0.1f); return 0; }
                    break;
                case VK_SUBTRACT:
                case VK_OEM_MINUS:
                    if (ctrl) { ApplyZoom(p, p->zoomLevel - 0.1f); return 0; }
                    break;
                case '0':
                    if (ctrl) { ApplyZoom(p, 1.0f); return 0; }
                    break;
            }
            break;
        }

        case WM_TIMER: {
            if (!p) break;
            if (wp == 1) {
                // 50 ms tick — advance animated frames and repaint if any
                // image moved on. Only invalidates the document area, the
                // search overlay (a child window) keeps its own paint.
                if (p->renderer.TickAnimation(50)) {
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            if (wp == 2) {
                // Deferred Lister-placement restore. Fires once, ~50 ms
                // after ListLoad, by which time TC has finished its own
                // window setup and any maximize-on-open behaviour.
                KillTimer(hwnd, 2);
                if (p->parentList && !p->placementApplied) {
                    p->placementApplied = true;
                    LoadWindowPlacement(p->parentList);
                }
                return 0;
            }
            break;
        }

        case WM_LBUTTONDOWN: {
            if (!p) break;
            int x = GET_X_LPARAM(lp);
            int y = GET_Y_LPARAM(lp);
            int yDoc = y + p->scrollY;
            // Link click takes priority over selection.
            std::wstring url = p->renderer.HitTestLink(x, yDoc);
            if (!url.empty()) {
                // 1) Pure in-document anchor — scroll to the heading.
                if (!url.empty() && url[0] == L'#') {
                    int targetY = 0;
                    if (p->renderer.FindAnchor(url.substr(1), &targetY)) {
                        int newScroll = targetY - 12;
                        if (newScroll < 0) newScroll = 0;
                        if (newScroll > p->contentH - p->clientH)
                            newScroll = std::max(0, p->contentH - p->clientH);
                        p->scrollY = newScroll;
                        UpdateScrollbar(p);
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    return 0;
                }
                // 2) Detect external schemes — http(s), mailto, ftp, etc.
                auto hasScheme = [](const std::wstring& s) {
                    size_t colon = s.find(L':');
                    if (colon == std::wstring::npos || colon == 0) return false;
                    for (size_t i = 0; i < colon; ++i) {
                        wchar_t c = s[i];
                        bool ok = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z')
                               || (c >= L'0' && c <= L'9') || c == L'+' || c == L'-' || c == L'.';
                        if (!ok) return false;
                    }
                    return true;
                };
                if (hasScheme(url)) {
                    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    return 0;
                }
                // 3) Relative path — resolve against the document base path
                //    and split off any "#fragment". If the resolved file
                //    happens to be the document we're already viewing, the
                //    fragment alone is enough to navigate.
                std::wstring filePart = url;
                std::wstring fragment;
                size_t hashPos = filePart.find(L'#');
                if (hashPos != std::wstring::npos) {
                    fragment = filePart.substr(hashPos + 1);
                    filePart = filePart.substr(0, hashPos);
                }
                // Replace forward slashes with backslashes for Windows.
                for (wchar_t& c : filePart) if (c == L'/') c = L'\\';
                std::wstring base = p->renderer.GetBasePath();
                if (!base.empty() && base.back() != L'\\' && base.back() != L'/')
                    base.push_back(L'\\');
                std::wstring resolved;
                if (filePart.empty()) {
                    // "#frag" only — treated as same-doc anchor (covered above
                    // already, but leave for safety).
                    if (!fragment.empty()) {
                        int targetY = 0;
                        if (p->renderer.FindAnchor(fragment, &targetY)) {
                            p->scrollY = std::max(0, targetY - 12);
                            UpdateScrollbar(p);
                            InvalidateRect(hwnd, nullptr, FALSE);
                        }
                        return 0;
                    }
                } else {
                    resolved = base + filePart;
                }
                if (!resolved.empty() &&
                    GetFileAttributesW(resolved.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    ShellExecuteW(nullptr, L"open", resolved.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    return 0;
                }
                // 4) Last-resort: hand the original string to the shell.
                ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            }
            int op = -1, ch = 0;
            if (p->renderer.HitTestText(x, yDoc, &op, &ch)) {
                p->selAnchorOp = op;
                p->selAnchorChar = ch;
                p->selCaretOp = op;
                p->selCaretChar = ch;
                p->renderer.SetSelection(op, ch, op, ch);
                p->selecting = true;
                SetCapture(hwnd);
            } else {
                p->renderer.ClearSelection();
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            SetFocus(hwnd);
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (!p) break;
            int x = GET_X_LPARAM(lp);
            int y = GET_Y_LPARAM(lp);
            int yDoc = y + p->scrollY;
            if (p->selecting && (wp & MK_LBUTTON)) {
                // Auto-scroll if dragging past the edges
                if (y < 0)              ScrollBy(p, -p->lineStep);
                else if (y > p->clientH) ScrollBy(p, p->lineStep);
                int op = -1, ch = 0;
                if (p->renderer.HitTestText(x, yDoc, &op, &ch)) {
                    p->selCaretOp = op;
                    p->selCaretChar = ch;
                    p->renderer.SetSelection(p->selAnchorOp, p->selAnchorChar,
                                              p->selCaretOp, p->selCaretChar);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            // Hover cursor: hand over links, I-beam over text, arrow elsewhere.
            std::wstring url = p->renderer.HitTestLink(x, yDoc);
            static thread_local int s_lastCursor = 0;
            int want = url.empty() ? 0 : 1;
            if (want != s_lastCursor) {
                s_lastCursor = want;
                SetCursor(LoadCursor(nullptr, want ? IDC_HAND : IDC_IBEAM));
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            if (!p) break;
            if (p->selecting) {
                p->selecting = false;
                ReleaseCapture();
                if (p->selAnchorOp == p->selCaretOp
                    && p->selAnchorChar == p->selCaretChar) {
                    p->renderer.ClearSelection();
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        }

        case WM_SETCURSOR: {
            if (!p) break;
            DWORD pos = GetMessagePos();
            POINT pt = { GET_X_LPARAM(pos), GET_Y_LPARAM(pos) };
            ScreenToClient(hwnd, &pt);
            std::wstring url = p->renderer.HitTestLink(pt.x, pt.y + p->scrollY);
            if (!url.empty()) SetCursor(LoadCursor(nullptr, IDC_HAND));
            else              SetCursor(LoadCursor(nullptr, IDC_IBEAM));
            return TRUE;
        }

        case WM_SETFOCUS:
            return 0;

        case WM_DESTROY:
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Plugin exports
// ---------------------------------------------------------------------------
extern "C" {

__declspec(dllexport)
void __stdcall ListGetDetectString(char* DetectString, int maxlen) {
    static const char* s = "EXT=\"MD\" | EXT=\"MARKDOWN\" | EXT=\"MKD\" | EXT=\"MDOWN\" | EXT=\"MDX\"";
    lstrcpynA(DetectString, s, maxlen);
}

static HWND DoLoad(HWND parent, const std::wstring& path) {
    RegisterClassesOnce();

    PluginInst* p = new PluginInst();
    p->parentList = parent;
    p->filePath = path;

    // Read & decode file
    auto raw = ReadFileBinary(path, 64 * 1024 * 1024); // 64 MB cap
    std::string utf8 = md::DecodeToUtf8(raw);
    p->doc = md::Parse(utf8, ParentDir(path));
    p->renderer.SetDocument(p->doc);   // copy — keep p->doc for PDF export
    // Kick off any remote (http/https) image fetches in parallel before
    // we lay out — Layout reads natural dimensions from the cached files,
    // so anything that came back in time gets sized correctly. The cap
    // bounds total wall-clock spent here regardless of how many remote
    // images the document references.
    p->renderer.PrefetchRemoteImages(5000);

    RECT pr; GetClientRect(parent, &pr);
    HWND hwnd = CreateWindowExW(
        0,
        kClassName, L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL,
        0, 0, pr.right - pr.left, pr.bottom - pr.top,
        parent, nullptr, g_hInstance, p);
    if (!hwnd) {
        delete p;
        return nullptr;
    }
    RelayoutAll(p);
    SetFocus(hwnd);
    // Restore Lister window placement from MDViewer.ini (if present).
    //   * If the saved state is maximized, apply immediately — the user
    //     wants the window maximized and TC's own auto-maximize would
    //     produce the same end state anyway, so there's no flicker and
    //     no reason to wait.
    //   * If the saved state is normal, defer via timer so we land
    //     AFTER any TC auto-maximize call.
    bool wantNormal = false;
    {
        std::wstring ini = GetIniPath();
        if (GetFileAttributesW(ini.c_str()) != INVALID_FILE_ATTRIBUTES) {
            int m = GetPrivateProfileIntW(L"Window", L"Maximized", -1, ini.c_str());
            if (m == 1) {
                LoadWindowPlacement(parent);
                p->placementApplied = true;
            } else if (m == 0) {
                wantNormal = true;
            }
        }
    }
    if (wantNormal) {
        SetTimer(hwnd, 2, 50, nullptr);
    }
    // Start the animation tick unconditionally. Image loading is now
    // lazy (defers GDI+ decode until Paint touches the image) so we don't
    // yet know whether the document contains an animated GIF — but the
    // tick is cheap when there's nothing to advance (TickAnimation just
    // returns false), so we don't need to gate it.
    SetTimer(hwnd, 1, 50, nullptr);
    return hwnd;
}

__declspec(dllexport)
HWND __stdcall ListLoad(HWND ParentWin, char* FileToLoad, int /*ShowFlags*/) {
    int n = MultiByteToWideChar(CP_ACP, 0, FileToLoad, -1, nullptr, 0);
    if (n <= 0) return nullptr;
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_ACP, 0, FileToLoad, -1, w.data(), n);
    return DoLoad(ParentWin, w);
}

__declspec(dllexport)
HWND __stdcall ListLoadW(HWND ParentWin, WCHAR* FileToLoad, int /*ShowFlags*/) {
    return DoLoad(ParentWin, FileToLoad ? std::wstring(FileToLoad) : std::wstring());
}

__declspec(dllexport)
void __stdcall ListCloseWindow(HWND ListWin) {
    if (!ListWin) return;
    PluginInst* p = (PluginInst*)GetWindowLongPtrW(ListWin, GWLP_USERDATA);
    // Persist Lister window placement before tearing down.
    if (p && p->parentList) SaveWindowPlacement(p->parentList);
    KillTimer(ListWin, 1);
    DestroyWindow(ListWin);
    delete p;
}

__declspec(dllexport)
int __stdcall ListSearchTextW(HWND ListWin, WCHAR* SearchString, int SearchParameter) {
    if (!ListWin || !SearchString) return LISTPLUGIN_ERROR;
    PluginInst* p = (PluginInst*)GetWindowLongPtrW(ListWin, GWLP_USERDATA);
    if (!p) return LISTPLUGIN_ERROR;
    bool findNext = (SearchParameter & lcs_findfirst) == 0;
    bool back     = (SearchParameter & lcs_backwards) != 0;

    std::wstring q = SearchString;
    int n = p->renderer.ApplySearch(q, false);
    if (n <= 0) return LISTPLUGIN_ERROR;

    int idx;
    if (!findNext) idx = 0;
    else if (back) {
        idx = p->renderer.GetCurrentMatch() - 1;
        if (idx < 0) idx = n - 1;
    } else {
        idx = p->renderer.GetCurrentMatch() + 1;
        if (idx >= n) idx = 0;
    }
    if (!p->searchOpen) OpenSearch(p);
    SetWindowTextW(p->searchEdit, q.c_str());
    GotoMatch(p, idx);
    return LISTPLUGIN_OK;
}

__declspec(dllexport)
int __stdcall ListSearchText(HWND ListWin, char* SearchString, int SearchParameter) {
    if (!SearchString) return LISTPLUGIN_ERROR;
    int n = MultiByteToWideChar(CP_ACP, 0, SearchString, -1, nullptr, 0);
    if (n <= 0) return LISTPLUGIN_ERROR;
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_ACP, 0, SearchString, -1, w.data(), n);
    return ListSearchTextW(ListWin, w.data(), SearchParameter);
}

__declspec(dllexport)
int __stdcall ListSendCommand(HWND ListWin, int Command, int /*Parameter*/) {
    if (!ListWin) return LISTPLUGIN_ERROR;
    PluginInst* p = (PluginInst*)GetWindowLongPtrW(ListWin, GWLP_USERDATA);
    if (!p) return LISTPLUGIN_ERROR;
    switch (Command) {
        case lc_copy: {
            std::wstring t = p->renderer.HasSelection()
                ? p->renderer.GetSelectionText()
                : p->renderer.GetPlainText();
            CopyToClipboard(ListWin, t);
            return LISTPLUGIN_OK;
        }
        case lc_selectall: {
            // TC sends this when the user presses Ctrl+A in Lister.
            int lastOp = 0, lastChar = 0;
            if (p->renderer.HitTestText(INT_MAX / 2, INT_MAX / 2, &lastOp, &lastChar)) {
                p->renderer.SetSelection(0, 0, lastOp, INT_MAX / 2);
                InvalidateRect(ListWin, nullptr, FALSE);
            }
            return LISTPLUGIN_OK;
        }
    }
    return LISTPLUGIN_ERROR;
}

__declspec(dllexport)
void __stdcall ListSetDefaultParams(ListDefaultParamStruct* /*dps*/) {
    // nothing to do
}

} // extern "C"

// ---------------------------------------------------------------------------
// DLL entry
// ---------------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInstance = h;
        DisableThreadLibraryCalls(h);
    }
    return TRUE;
}
