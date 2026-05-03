// parser.h -- Markdown parser producing a structured block AST.
// No HTML output; the renderer consumes Block nodes directly.
#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace md {

// Inline style flags (bitwise OR).
enum InlineStyle : uint32_t {
    S_NONE   = 0,
    S_BOLD   = 1u << 0,
    S_ITALIC = 1u << 1,
    S_CODE   = 1u << 2,
    S_STRIKE = 1u << 3,
    S_LINK   = 1u << 4,
};

enum class InlineKind {
    Text,
    Image,
    LineBreak,
};

struct Inline {
    InlineKind kind = InlineKind::Text;
    std::wstring text;          // text to display (Text); alt text (Image)
    uint32_t     style   = 0;   // OR of InlineStyle
    std::wstring linkUrl;       // populated when S_LINK
    std::wstring imagePath;     // when kind == Image: absolute or resolvable path
    // HTML-style requested image size. 0 = unspecified (use natural).
    int  imageReqW = 0;
    int  imageReqH = 0;
    bool imageWPct = false;     // imageReqW is a percentage of container width
    bool imageHPct = false;
};

enum class BType {
    Empty,
    Heading,           // headingLevel 1..6, inlines
    Paragraph,         // inlines
    CodeBlock,         // codeText (multi-line), codeLang
    BlockQuote,        // inlines (one merged block per quote group)
    UnorderedList,     // listItems
    OrderedList,       // listItems, listStartIndex
    HorizontalRule,
    Table,             // tableHeader (one row), tableBody (rows), tableAligns
    ImageBlock,        // imagePath, imageAlt (paragraph that is just an image)
};

struct ListItem {
    bool                taskMarker  = false;
    bool                taskChecked = false;
    std::vector<Inline> inlines;
};

struct TableCell {
    std::vector<Inline> inlines;
};

struct TableRow {
    std::vector<TableCell> cells;
};

enum TableAlign { ALIGN_LEFT = 0, ALIGN_CENTER = 1, ALIGN_RIGHT = 2 };

struct Block {
    BType type = BType::Empty;

    // Heading
    int headingLevel = 0;

    // Inlines for Heading / Paragraph / BlockQuote
    std::vector<Inline> inlines;

    // CodeBlock
    std::wstring codeText;     // raw text incl. \n separators
    std::wstring codeLang;

    // List (unordered / ordered)
    std::vector<ListItem> listItems;
    int listStartIndex = 1;    // for ordered

    // Table
    TableRow                tableHeader;
    std::vector<TableRow>   tableBody;
    std::vector<int>        tableAligns;

    // Image block
    std::wstring imagePath;
    std::wstring imageAlt;
    int  imageReqW = 0;
    int  imageReqH = 0;
    bool imageWPct = false;
    bool imageHPct = false;
};

struct Document {
    std::vector<Block> blocks;
    std::wstring       basePath;   // for resolving relative image paths
};

// Parse UTF-8 / UTF-16 (caller hands UTF-8 text).
// basePath is the directory containing the source file; used to resolve
// relative image paths.
Document Parse(const std::string& utf8text, const std::wstring& basePath);

// Convert an arbitrary file-content blob to UTF-8. Detects UTF-8 BOM,
// UTF-16 LE/BE BOM; otherwise tries valid-UTF-8 and falls back to CP_ACP.
std::string DecodeToUtf8(const std::vector<unsigned char>& raw);

// UTF-8 -> UTF-16 (Win32 wide).
std::wstring Utf8ToWide(const std::string& s);

} // namespace md
