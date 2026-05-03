// renderer.h -- Native layout + painting for MDViewer.
//
// Responsibilities:
//   * Take a parsed Document, pre-compute a flat list of "paint commands"
//     (positioned text runs, images, rectangles, lines).
//   * Paint a vertical slice of the laid-out document onto an HDC.
//   * Search across the rendered text and report match positions for the
//     viewer (so it can scroll to them) and for the painter (so it can
//     overlay highlight rectangles).
//   * Hand back the plain text of the whole document for clipboard copy.
#pragma once

#include <windows.h>
#include <vector>
#include <string>
#include "parser.h"

// Forward-declare Gdiplus types so we don't have to include the full
// gdiplus.h in this public header.
namespace Gdiplus { class FontFamily; class Font; class Graphics; class Bitmap; }

namespace md {

// Index for the cached fonts. Keep in sync with renderer.cpp.
enum FontKind {
    FK_BODY        = 0,
    FK_BODY_B      = 1,
    FK_BODY_I      = 2,
    FK_BODY_BI     = 3,
    FK_CODE        = 4,
    FK_CODE_B      = 5,
    FK_CODE_I      = 6,
    FK_H1          = 7,
    FK_H2          = 8,
    FK_H3          = 9,
    FK_H4          = 10,
    FK_H5          = 11,
    FK_H6          = 12,
    FK_EMOJI       = 13,
    FK_COUNT       = 14,
};

enum DrawKind {
    D_TEXT,
    D_IMAGE,
    D_RULE,
    D_RECT,         // filled rectangle (background)
    D_LINE,         // single-pixel line (or thin band) – table border, blockquote bar
    D_CHECKBOX,     // task-list checkbox
};

struct DrawOp {
    int      kind = D_TEXT;
    int      x = 0, y = 0;        // top-left in document coords
    int      w = 0, h = 0;
    int      font = FK_BODY;
    COLORREF color = RGB(36, 41, 47);
    COLORREF bg    = 0xFFFFFFFF;  // sentinel = transparent
    int      penWidth = 1;
    bool     underline = false;
    bool     strike = false;
    bool     checked = false;     // for checkbox
    int      imageIndex = -1;     // for D_IMAGE: index into image cache
    std::wstring text;            // for D_TEXT
    std::wstring linkUrl;         // optional link associated with this run
    // Search support: index of the run in the global "text run" sequence,
    // for fast hit-testing during search highlight overlay.
    int      runId = -1;
};

struct SearchHit {
    int  opIndex;       // index into ops_ for the run that contains this hit
    int  charStart;     // char offset within the op's text
    int  charLen;
    int  globalIndex;   // 0..N-1 over all hits in document order
};

class Renderer {
public:
    Renderer();
    ~Renderer();

    // Set/replace document. Layout will be recomputed on next Layout().
    void SetDocument(Document doc);

    // Compute layout for the given content width (px). Loads images from
    // disk, measures text, fills ops_ with positioned draw commands and
    // sets totalHeight_.
    // Must be called whenever width changes.
    void Layout(HWND hwnd, int contentWidth);

    int  GetTotalHeight() const { return totalHeight_; }
    int  GetContentWidth() const { return contentWidth_; }

    // Paint slice [scrollY, scrollY + clientH) into hdc. The hdc origin
    // corresponds to (0,0) of the client area.
    void Paint(HDC hdc, int scrollY, int clientW, int clientH);

    // ------ Search -------------------------------------------------------
    // Returns number of hits.
    int  ApplySearch(const std::wstring& q, bool caseSensitive);
    void ClearSearch();
    int  GetMatchCount() const  { return (int)hits_.size(); }
    int  GetCurrentMatch() const{ return currentHit_; }
    void SetCurrentMatch(int idx);
    // Returns Y position of the given match (-1 if none); also fills hOut.
    bool GetMatchYRange(int idx, int* outY, int* outH) const;

    // Plain text representation (for clipboard "select all").
    std::wstring GetPlainText() const;

    // Find link URL at given document coordinates, if any. Returns empty
    // string if not over a link.
    std::wstring HitTestLink(int x, int yDoc) const;

    // Synchronously fetch every http/https image referenced by the loaded
    // Document, in parallel, with a wall-clock cap. Already-cached entries
    // are skipped, so reopening the same file is free. Call once after
    // SetDocument and before Layout so Layout can read dimensions from the
    // cached files.
    void PrefetchRemoteImages(int totalTimeoutMs = 5000);

    // Resolve a heading anchor (e.g. "section-name" — without the leading
    // '#') to a document Y position. Returns true and writes Y into *outY
    // if a heading with the matching normalised id exists.
    bool FindAnchor(const std::wstring& anchorId, int* outY) const;

    // Document base path — used by callers that need to resolve relative
    // links such as "other.md" or "doc/picture.png".
    const std::wstring& GetBasePath() const { return basePath_; }

    // ------ Selection ----------------------------------------------------
    // Hit-test (x, yDoc) and return (opIndex, charOffset). Returns false
    // if not over any text run.
    bool HitTestText(int x, int yDoc, int* outOpIndex, int* outCharOffset) const;

    // Set the selection range. Both ends are (opIndex, charOffset).
    // Pass opIndex < 0 to clear.
    void SetSelection(int aOp, int aChar, int bOp, int bChar);
    void ClearSelection();
    bool HasSelection() const;
    std::wstring GetSelectionText() const;
    // True if any selection is currently being painted.
    bool HasSelectionAtPaint() const { return selA_op_ >= 0 && selB_op_ >= 0
                                              && !(selA_op_ == selB_op_ && selA_char_ == selB_char_); }

    // Set zoom factor (1.0 = default). Triggers font recreation on next
    // Layout. Reasonable range 0.5 .. 3.0.
    void SetZoom(float z);
    float GetZoom() const { return zoom_; }

    // Helpers used during layout. Public so file-local helpers can call
    // them without befriending; not part of the intended outside API.
    int  LoadImageFromFile_Public(const std::wstring& path);
    int  GetImageWidth_Public(int idx) const;
    int  GetImageHeight_Public(int idx) const;
    void EnsureImageRenderedAt_Public(int idx, int dispW, int dispH);
    Gdiplus::Font* GetGdiplusFont_Public(int idx) const;
    int  GetLineHeight_Public(int idx) const { return (idx >= 0 && idx < FK_COUNT) ? lineHeight_[idx] : 0; }
    int  GetAscent_Public(int idx) const { return (idx >= 0 && idx < FK_COUNT) ? ascent_[idx] : 0; }
    int  GetDescent_Public(int idx) const { return (idx >= 0 && idx < FK_COUNT) ? descent_[idx] : 0; }
    int  GetAvgChar_Public(int idx) const { return (idx >= 0 && idx < FK_COUNT) ? avgChar_[idx] : 0; }

    // Animation: advance frame timers by elapsedMs. Returns true if any
    // animated image moved to a new frame and the viewer needs a redraw.
    bool TickAnimation(unsigned int elapsedMs);
    // True if at least one loaded image has more than one frame.
    bool HasAnimation() const;

private:
    Document doc_;
    int      contentWidth_ = 0;
    int      totalHeight_ = 0;
    float    zoom_ = 1.0f;

    Gdiplus::FontFamily* familyBody_ = nullptr;
    Gdiplus::FontFamily* familyMono_ = nullptr;
    Gdiplus::FontFamily* familyEmoji_ = nullptr;
    Gdiplus::Font*       gpFonts_[FK_COUNT] = {};
    float                fontSizePx_[FK_COUNT] = {};
    int      lineHeight_[FK_COUNT] = {};
    int      ascent_[FK_COUNT] = {};
    int      descent_[FK_COUNT] = {};
    int      avgChar_[FK_COUNT] = {};

    // DirectWrite text formats — only the emoji format is actually used
    // for color rendering; we keep the others as a future hook.
    void* dwEmojiFormat_ = nullptr;   // IDWriteTextFormat*

    std::vector<DrawOp> ops_;

    // Per-loaded-image entry. We read the natural dimensions from the
    // file header during Layout (cheap — bytes 6..9 of GIF, IHDR of PNG,
    // SOF marker of JPEG) but defer the full GDI+ decode until Paint
    // actually needs the bitmap. That lets ListLoad return quickly even
    // for READMEs with many large GIFs.
    struct ImageEntry {
        std::wstring path;                 // resolved on-disk path
        bool         loaded = false;
        bool         loadFailed = false;
        Gdiplus::Bitmap* bmp = nullptr;
        int          width  = 0;
        int          height = 0;
        int          totalFrames = 1;
        bool         hasTimeDim = false;
        std::vector<unsigned int> delaysMs;
        unsigned int currentFrame = 0;
        unsigned int accumMs      = 0;
        HBITMAP      cachedHbm    = nullptr;
        int          cachedFrame  = -1;
        int          cachedDispW  = 0;
        int          cachedDispH  = 0;
    };
    std::vector<ImageEntry> images_;

    std::vector<SearchHit> hits_;
    int                    currentHit_ = -1;
    std::wstring           lastQuery_;
    bool                   lastCaseSens_ = false;

    // Heading anchor map: normalised id -> document Y, populated during
    // Layout for every Heading block. Used by FindAnchor for #section
    // link navigation.
    struct AnchorEntry {
        std::wstring id;
        int          y;
    };
    std::vector<AnchorEntry> anchors_;

    // Document base path (from Document::basePath). Public via
    // GetBasePath(); used by callers to resolve relative file links.
    std::wstring basePath_;

    // Selection (kept normalized so A <= B in document order).
    int  selA_op_ = -1;
    int  selA_char_ = 0;
    int  selB_op_ = -1;
    int  selB_char_ = 0;

    void DestroyFonts();
    void CreateFonts(HWND hwnd);
    void DestroyImages();
    int  LoadImageFromFile(const std::wstring& path);
    void LoadImageNow(int idx);
    void RecomputeHits();
};

} // namespace md
