// parser.cpp -- Markdown -> Block AST.
//
// Pragmatic GFM-ish dialect:
//   * ATX and Setext headings
//   * Paragraphs with hard / soft line breaks
//   * **bold**, __bold__, *italic*, _italic_, `code`, ~~strike~~
//   * [text](url), <http://autolink>
//   * ![alt](src) (inline OR block when alone in a paragraph)
//   * Fenced code blocks ``` and ~~~
//   * Unordered (-, *, +) and ordered (N. or N)) lists
//   * Task list items: - [ ] / - [x]
//   * > Blockquotes
//   * Horizontal rules: ---, ***, ___
//   * GFM tables with :--- alignment
//   * Backslash escapes: \* \_ \` etc.

#include "parser.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <cstring>
#include <string>

namespace md {

// ---------- UTF helpers -----------------------------------------------------

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w; w.resize(n);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    std::string s; s.resize(n);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static bool IsValidUtf8(const unsigned char* data, size_t len) {
    size_t i = 0;
    while (i < len) {
        unsigned char c = data[i];
        size_t need = 0;
        if (c < 0x80) { i++; continue; }
        else if ((c & 0xE0) == 0xC0) need = 1;
        else if ((c & 0xF0) == 0xE0) need = 2;
        else if ((c & 0xF8) == 0xF0) need = 3;
        else return false;
        if (i + need >= len) return false;
        for (size_t k = 1; k <= need; ++k) {
            if ((data[i + k] & 0xC0) != 0x80) return false;
        }
        i += need + 1;
    }
    return true;
}

std::string DecodeToUtf8(const std::vector<unsigned char>& raw) {
    if (raw.empty()) return "";
    // UTF-8 BOM
    if (raw.size() >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) {
        return std::string((const char*)raw.data() + 3, raw.size() - 3);
    }
    // UTF-16 LE BOM
    if (raw.size() >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        const wchar_t* w = reinterpret_cast<const wchar_t*>(raw.data() + 2);
        size_t wlen = (raw.size() - 2) / 2;
        return WideToUtf8(std::wstring(w, wlen));
    }
    // UTF-16 BE BOM
    if (raw.size() >= 2 && raw[0] == 0xFE && raw[1] == 0xFF) {
        std::wstring tmp; tmp.reserve((raw.size() - 2) / 2);
        for (size_t i = 2; i + 1 < raw.size(); i += 2) {
            tmp.push_back((wchar_t)((raw[i] << 8) | raw[i + 1]));
        }
        return WideToUtf8(tmp);
    }
    // Valid UTF-8?
    if (IsValidUtf8(raw.data(), raw.size())) {
        return std::string((const char*)raw.data(), raw.size());
    }
    // Fall back: assume system ANSI codepage.
    int n = MultiByteToWideChar(CP_ACP, 0, (const char*)raw.data(), (int)raw.size(), nullptr, 0);
    if (n <= 0) return std::string((const char*)raw.data(), raw.size());
    std::wstring w; w.resize(n);
    MultiByteToWideChar(CP_ACP, 0, (const char*)raw.data(), (int)raw.size(), w.data(), n);
    return WideToUtf8(w);
}

// ---------- Tiny utilities --------------------------------------------------

namespace {

std::vector<std::string> SplitLines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\r') {
            out.push_back(std::move(cur));
            cur.clear();
            if (i + 1 < s.size() && s[i + 1] == '\n') ++i;
        } else if (c == '\n') {
            out.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(std::move(cur));
    return out;
}

bool IsBlank(const std::string& s) {
    for (char c : s) {
        if (c != ' ' && c != '\t') return false;
    }
    return true;
}

std::string Rtrim(const std::string& s) {
    size_t e = s.size();
    while (e > 0 && (s[e-1] == ' ' || s[e-1] == '\t')) --e;
    return s.substr(0, e);
}

int LeadingSpaces(const std::string& s) {
    int n = 0;
    for (char c : s) {
        if (c == ' ') ++n;
        else if (c == '\t') n += 4 - (n % 4);
        else break;
    }
    return n;
}

bool IsHorizontalRule(const std::string& line) {
    int count = 0;
    char marker = 0;
    for (char c : line) {
        if (c == ' ' || c == '\t') continue;
        if (c == '-' || c == '*' || c == '_') {
            if (marker == 0) marker = c;
            else if (c != marker) return false;
            ++count;
        } else {
            return false;
        }
    }
    return count >= 3;
}

// Recognize ATX heading "# ..." up to "###### ...". Returns level or 0.
int AtxHeadingLevel(const std::string& line, size_t* outContentStart) {
    size_t i = 0;
    int    indent = 0;
    while (i < line.size() && line[i] == ' ' && indent < 3) { ++i; ++indent; }
    int level = 0;
    while (i < line.size() && line[i] == '#' && level < 6) { ++i; ++level; }
    if (level == 0) return 0;
    if (i < line.size() && line[i] != ' ' && line[i] != '\t') return 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (outContentStart) *outContentStart = i;
    return level;
}

// Strip trailing # ... # closer from ATX headings.
std::string StripAtxTail(const std::string& s) {
    std::string t = Rtrim(s);
    // Strip trailing run of # if preceded by space.
    size_t e = t.size();
    while (e > 0 && t[e-1] == '#') --e;
    if (e < t.size() && (e == 0 || t[e-1] == ' ')) {
        t = Rtrim(t.substr(0, e));
    }
    return t;
}

// Setext underline? returns 1 for '=' (h1), 2 for '-' (h2), 0 otherwise.
int SetextUnderline(const std::string& line) {
    std::string t = Rtrim(line);
    int indent = 0;
    while (indent < (int)t.size() && t[indent] == ' ' && indent < 3) ++indent;
    if (indent == (int)t.size()) return 0;
    char m = t[indent];
    if (m != '=' && m != '-') return 0;
    for (size_t i = indent + 1; i < t.size(); ++i) {
        if (t[i] != m) return 0;
    }
    return (m == '=') ? 1 : 2;
}

// Fenced code: ``` or ~~~. Returns marker char (0 if none) and length.
char FenceChar(const std::string& line, int* outLen, std::string* outInfo) {
    size_t i = 0;
    int indent = 0;
    while (i < line.size() && line[i] == ' ' && indent < 3) { ++i; ++indent; }
    if (i >= line.size()) return 0;
    char m = line[i];
    if (m != '`' && m != '~') return 0;
    int n = 0;
    while (i < line.size() && line[i] == m) { ++i; ++n; }
    if (n < 3) return 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (outInfo) *outInfo = Rtrim(line.substr(i));
    if (outLen) *outLen = n;
    return m;
}

// Closing fence?
bool IsClosingFence(const std::string& line, char marker, int minLen) {
    size_t i = 0;
    int indent = 0;
    while (i < line.size() && line[i] == ' ' && indent < 3) { ++i; ++indent; }
    int n = 0;
    while (i < line.size() && line[i] == marker) { ++i; ++n; }
    if (n < minLen) return false;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    return i == line.size();
}

// Match an unordered list bullet at the start: '-', '*', or '+' followed by
// space/tab. Returns the offset of the content start (after the marker and
// the single mandatory space), or -1.
int UnorderedBullet(const std::string& line, int* outIndent) {
    int i = 0, indent = 0;
    while (i < (int)line.size() && line[i] == ' ' && indent < 3) { ++i; ++indent; }
    if (outIndent) *outIndent = indent;
    if (i >= (int)line.size()) return -1;
    char c = line[i];
    if (c != '-' && c != '*' && c != '+') return -1;
    if (i + 1 >= (int)line.size()) return -1;
    if (line[i+1] != ' ' && line[i+1] != '\t') return -1;
    return i + 2;
}

// Match an ordered list bullet "N." or "N)". Returns content start, or -1.
int OrderedBullet(const std::string& line, int* outIndent, int* outNumber) {
    int i = 0, indent = 0;
    while (i < (int)line.size() && line[i] == ' ' && indent < 3) { ++i; ++indent; }
    if (outIndent) *outIndent = indent;
    int j = i;
    while (j < (int)line.size() && line[j] >= '0' && line[j] <= '9') ++j;
    if (j == i || j - i > 9) return -1;
    if (j >= (int)line.size()) return -1;
    if (line[j] != '.' && line[j] != ')') return -1;
    if (j + 1 >= (int)line.size()) return -1;
    if (line[j+1] != ' ' && line[j+1] != '\t') return -1;
    if (outNumber) *outNumber = std::atoi(line.substr(i, j-i).c_str());
    return j + 2;
}

// Detect task-list marker at start of list item content.
bool TaskMarker(const std::string& s, bool* checked, size_t* outAfter) {
    if (s.size() < 4) return false;
    if (s[0] != '[') return false;
    char c = s[1];
    if (c != ' ' && c != 'x' && c != 'X') return false;
    if (s[2] != ']') return false;
    if (s[3] != ' ' && s[3] != '\t') return false;
    if (checked)  *checked  = (c == 'x' || c == 'X');
    if (outAfter) *outAfter = 4;
    return true;
}

// True if line is a > blockquote line (with optional leading 0-3 spaces).
bool IsBlockquote(const std::string& line, size_t* contentStart) {
    size_t i = 0;
    int indent = 0;
    while (i < line.size() && line[i] == ' ' && indent < 3) { ++i; ++indent; }
    if (i >= line.size() || line[i] != '>') return false;
    ++i;
    if (i < line.size() && line[i] == ' ') ++i;
    if (contentStart) *contentStart = i;
    return true;
}

// Detect "| col1 | col2 |" header / row -- presence of pipes.
bool LooksLikeTableRow(const std::string& s) {
    return s.find('|') != std::string::npos;
}

// Split a table row by '|' respecting backslash escapes. Trims cells.
// Strips one leading + trailing pipe if present.
std::vector<std::string> SplitTableRow(const std::string& s) {
    std::string trimmed = Rtrim(s);
    size_t a = 0;
    while (a < trimmed.size() && (trimmed[a] == ' ' || trimmed[a] == '\t')) ++a;
    if (a < trimmed.size() && trimmed[a] == '|') ++a;
    size_t b = trimmed.size();
    if (b > a && trimmed[b-1] == '|' && (b < 2 || trimmed[b-2] != '\\')) --b;

    std::vector<std::string> out;
    std::string cur;
    for (size_t i = a; i < b; ++i) {
        char c = trimmed[i];
        if (c == '\\' && i + 1 < b && trimmed[i+1] == '|') {
            cur.push_back('|');
            ++i;
        } else if (c == '|') {
            // Trim
            size_t s0 = 0, s1 = cur.size();
            while (s0 < s1 && (cur[s0] == ' ' || cur[s0] == '\t')) ++s0;
            while (s1 > s0 && (cur[s1-1] == ' ' || cur[s1-1] == '\t')) --s1;
            out.push_back(cur.substr(s0, s1 - s0));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    size_t s0 = 0, s1 = cur.size();
    while (s0 < s1 && (cur[s0] == ' ' || cur[s0] == '\t')) ++s0;
    while (s1 > s0 && (cur[s1-1] == ' ' || cur[s1-1] == '\t')) --s1;
    out.push_back(cur.substr(s0, s1 - s0));
    return out;
}

// Detect alignment row: cells like "---", ":---", "---:", ":---:".
bool ParseAlignRow(const std::vector<std::string>& cells, std::vector<int>& aligns) {
    aligns.clear();
    if (cells.empty()) return false;
    for (const auto& cell : cells) {
        std::string c;
        for (char ch : cell) if (ch != ' ' && ch != '\t') c.push_back(ch);
        if (c.empty()) return false;
        bool left  = (c.front() == ':');
        bool right = (c.back()  == ':');
        size_t s0 = left ? 1 : 0;
        size_t s1 = right ? c.size() - 1 : c.size();
        if (s1 <= s0) return false;
        for (size_t i = s0; i < s1; ++i) {
            if (c[i] != '-') return false;
        }
        if (left && right) aligns.push_back(ALIGN_CENTER);
        else if (right)     aligns.push_back(ALIGN_RIGHT);
        else                aligns.push_back(ALIGN_LEFT);
    }
    return true;
}

// ---------- Inline parsing -------------------------------------------------

// Resolve a possibly-relative image path against base dir. Decodes
// percent-escapes, strips leading "./", normalizes slashes.
std::wstring ResolveImagePath(const std::string& src, const std::wstring& base) {
    std::wstring w = Utf8ToWide(src);
    // URL prefix?
    auto starts = [&](const wchar_t* p) {
        size_t n = wcslen(p);
        if (w.size() < n) return false;
        for (size_t i = 0; i < n; ++i) {
            wchar_t c = w[i];
            wchar_t pc = p[i];
            if (towlower(c) != towlower(pc)) return false;
        }
        return true;
    };
    if (starts(L"http://") || starts(L"https://") || starts(L"data:")) {
        // Pass through unchanged; renderer skips remote URLs.
        return w;
    }
    if (starts(L"file://")) {
        w.erase(0, 7);
        if (!w.empty() && w[0] == L'/' && w.size() >= 3 && w[2] == L':') w.erase(0, 1);
    }
    // Strip "./" or ".\\" prefixes
    while (w.size() >= 2 && w[0] == L'.' && (w[1] == L'/' || w[1] == L'\\')) w.erase(0, 2);
    // Already absolute? (drive letter or UNC)
    bool absolute = (w.size() >= 2 && w[1] == L':')
                  || (!w.empty() && (w[0] == L'\\' || w[0] == L'/'));
    if (absolute || base.empty()) {
        for (wchar_t& c : w) if (c == L'/') c = L'\\';
        return w;
    }
    std::wstring path = base;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
        path.push_back(L'\\');
    for (wchar_t& c : w) if (c == L'/') c = L'\\';
    return path + w;
}

// Parse HTML attribute value out of a tag-content string (everything
// between < and >, exclusive). Returns the value of the named attribute
// or empty string. Case-insensitive on the attribute name.
static std::wstring ParseHtmlAttr(const std::wstring& tag, const wchar_t* attrName) {
    size_t pos = 0;
    std::wstring lookup = attrName;
    for (auto& c : lookup) c = (wchar_t)towlower(c);
    while (pos < tag.size()) {
        while (pos < tag.size() && iswspace(tag[pos])) ++pos;
        size_t nameStart = pos;
        while (pos < tag.size()
               && (iswalnum(tag[pos]) || tag[pos] == L'-' || tag[pos] == L'_'))
            ++pos;
        std::wstring nm(tag.begin() + nameStart, tag.begin() + pos);
        for (auto& c : nm) c = (wchar_t)towlower(c);
        while (pos < tag.size() && iswspace(tag[pos])) ++pos;
        if (pos >= tag.size() || tag[pos] != L'=') {
            if (nm.empty() && pos < tag.size()) ++pos;
            continue;
        }
        ++pos;   // skip '='
        while (pos < tag.size() && iswspace(tag[pos])) ++pos;
        std::wstring val;
        if (pos < tag.size() && (tag[pos] == L'"' || tag[pos] == L'\'')) {
            wchar_t q = tag[pos++];
            while (pos < tag.size() && tag[pos] != q) val.push_back(tag[pos++]);
            if (pos < tag.size()) ++pos;
        } else {
            while (pos < tag.size() && !iswspace(tag[pos]) && tag[pos] != L'>'
                   && tag[pos] != L'/') {
                val.push_back(tag[pos++]);
            }
        }
        if (nm == lookup) return val;
    }
    return L"";
}

// True if `src[pos]` looks like the start of a HTML tag — '<' followed
// by a tag name (with optional '/'). Used to disambiguate autolinks
// (which also start with '<') from <img> / <br> / etc.
static bool LooksLikeHtmlTag(const std::wstring& src, size_t pos) {
    if (pos >= src.size() || src[pos] != L'<') return false;
    size_t i = pos + 1;
    if (i < src.size() && src[i] == L'/') ++i;
    size_t nameStart = i;
    while (i < src.size() && (iswalnum(src[i]) || src[i] == L'-')) ++i;
    if (i == nameStart) return false;
    if (i >= src.size()) return false;
    wchar_t c = src[i];
    return iswspace(c) || c == L'/' || c == L'>';
}

struct InlineParser {
    const std::wstring& src;
    const std::wstring& base;
    size_t pos = 0;
    uint32_t style = 0;
    std::vector<Inline> out;

    InlineParser(const std::wstring& s, const std::wstring& b) : src(s), base(b) {}

    void appendChar(wchar_t c) {
        if (out.empty()
            || out.back().kind != InlineKind::Text
            || out.back().style != style
            || (out.back().style & S_LINK) != 0) {
            Inline it;
            it.kind = InlineKind::Text;
            it.style = style;
            it.text.push_back(c);
            out.push_back(std::move(it));
        } else {
            out.back().text.push_back(c);
        }
    }
    void appendText(const std::wstring& s) {
        for (wchar_t c : s) appendChar(c);
    }

    bool tryEmphasis() {
        wchar_t m = src[pos];
        if (m != L'*' && m != L'_') return false;
        // Count run length
        size_t start = pos;
        while (pos < src.size() && src[pos] == m) ++pos;
        int runLen = (int)(pos - start);
        // Look ahead for matching close run
        size_t scan = pos;
        size_t closeStart = std::wstring::npos;
        int    closeLen   = 0;
        while (scan < src.size()) {
            wchar_t c = src[scan];
            if (c == L'\\' && scan + 1 < src.size()) { scan += 2; continue; }
            if (c == L'`') {
                size_t b = scan;
                while (scan < src.size() && src[scan] == L'`') ++scan;
                int bl = (int)(scan - b);
                // skip until matching backticks
                while (scan < src.size()) {
                    if (src[scan] == L'`') {
                        size_t b2 = scan;
                        while (scan < src.size() && src[scan] == L'`') ++scan;
                        if ((int)(scan - b2) == bl) break;
                    } else ++scan;
                }
                continue;
            }
            if (c == m) {
                size_t cs = scan;
                while (scan < src.size() && src[scan] == m) ++scan;
                int cl = (int)(scan - cs);
                // For underscore, require word boundary before close run.
                bool ok = true;
                if (m == L'_') {
                    if (scan < src.size() && (iswalnum(src[scan]))) ok = false;
                }
                if (ok && cl >= 1) {
                    closeStart = cs;
                    closeLen   = cl;
                    break;
                }
                continue;
            }
            ++scan;
        }
        if (closeStart == std::wstring::npos) {
            // Treat as literal
            for (int k = 0; k < runLen; ++k) appendChar(m);
            return true;
        }
        // Decide style: bold if run >=2, italic otherwise (greedy match)
        int useOpen  = std::min(runLen, std::min(closeLen, 3));
        int matchLen = useOpen;
        uint32_t addStyle = 0;
        if (matchLen >= 2) addStyle |= S_BOLD;
        if (matchLen == 1 || matchLen == 3) addStyle |= S_ITALIC;
        // Push leftover open markers as literal text
        for (int k = 0; k < runLen - matchLen; ++k) appendChar(m);

        size_t innerStart = pos;
        size_t innerEnd   = closeStart;
        std::wstring inner = src.substr(innerStart, innerEnd - innerStart);
        InlineParser ip(src, base);
        ip.pos = 0;
        ip.style = style | addStyle;
        // Hack: parse via separate parser
        InlineParser sub = InlineParser(inner, base);
        sub.style = style | addStyle;
        sub.pos = 0;
        // We need a parse over `inner`; emulate by temporarily swapping
        // src... simplest: call a helper.
        std::vector<Inline> innerOut = ParseInner(inner, ip.style, base);
        for (auto& x : innerOut) out.push_back(std::move(x));

        // Advance master cursor past closeStart + matchLen markers
        pos = closeStart + matchLen;
        // Push leftover close markers as literal
        for (int k = 0; k < closeLen - matchLen; ++k) appendChar(m);
        return true;
    }

    bool tryStrike() {
        if (pos + 1 >= src.size() || src[pos] != L'~' || src[pos+1] != L'~') return false;
        size_t close = src.find(L"~~", pos + 2);
        if (close == std::wstring::npos) return false;
        std::wstring inner = src.substr(pos + 2, close - (pos + 2));
        std::vector<Inline> innerOut = ParseInner(inner, style | S_STRIKE, base);
        for (auto& x : innerOut) out.push_back(std::move(x));
        pos = close + 2;
        return true;
    }

    bool tryInlineCode() {
        if (src[pos] != L'`') return false;
        size_t a = pos;
        while (pos < src.size() && src[pos] == L'`') ++pos;
        int    n = (int)(pos - a);
        size_t scan = pos;
        while (scan < src.size()) {
            if (src[scan] == L'`') {
                size_t b = scan;
                while (scan < src.size() && src[scan] == L'`') ++scan;
                if ((int)(scan - b) == n) {
                    std::wstring inner = src.substr(pos, b - pos);
                    // CommonMark trims one space on each side if both ends
                    // have one and content not all-spaces.
                    if (inner.size() >= 2 && inner.front() == L' ' && inner.back() == L' ') {
                        bool allSpace = true;
                        for (wchar_t c : inner) if (c != L' ') { allSpace = false; break; }
                        if (!allSpace) inner = inner.substr(1, inner.size() - 2);
                    }
                    Inline it;
                    it.kind = InlineKind::Text;
                    it.text = inner;
                    it.style = style | S_CODE;
                    out.push_back(std::move(it));
                    pos = scan;
                    return true;
                }
                continue;
            }
            ++scan;
        }
        // No closing -- emit backticks literally
        for (int k = 0; k < n; ++k) appendChar(L'`');
        return true;
    }

    bool tryLinkOrImage() {
        bool isImage = false;
        size_t start = pos;
        if (src[pos] == L'!') {
            if (pos + 1 >= src.size() || src[pos+1] != L'[') return false;
            isImage = true;
            ++pos;
        } else if (src[pos] != L'[') return false;
        // Find matching ']'
        size_t labelStart = pos + 1;
        int    depth = 1;
        size_t i = labelStart;
        while (i < src.size() && depth > 0) {
            wchar_t c = src[i];
            if (c == L'\\' && i + 1 < src.size()) { i += 2; continue; }
            if (c == L'[') ++depth;
            else if (c == L']') { --depth; if (depth == 0) break; }
            ++i;
        }
        if (i >= src.size() || depth != 0) { pos = start; appendChar(src[start]); ++pos; return true; }
        size_t labelEnd = i;
        if (labelEnd + 1 >= src.size() || src[labelEnd + 1] != L'(') {
            pos = start; appendChar(src[start]); ++pos; return true;
        }
        size_t urlStart = labelEnd + 2;
        // Walk to closing ')', allowing nested () as long as balanced and no spaces in URL part
        int    pdepth = 1;
        size_t j = urlStart;
        while (j < src.size() && pdepth > 0) {
            wchar_t c = src[j];
            if (c == L'\\' && j + 1 < src.size()) { j += 2; continue; }
            if (c == L'(') ++pdepth;
            else if (c == L')') { --pdepth; if (pdepth == 0) break; }
            ++j;
        }
        if (j >= src.size() || pdepth != 0) { pos = start; appendChar(src[start]); ++pos; return true; }
        std::wstring url = src.substr(urlStart, j - urlStart);
        // Trim and strip optional title "..."
        while (!url.empty() && (url.front() == L' ' || url.front() == L'\t')) url.erase(0, 1);
        while (!url.empty() && (url.back()  == L' ' || url.back()  == L'\t')) url.pop_back();
        // Title removal: find first space outside of <>.
        std::wstring urlOnly = url;
        size_t sp = std::wstring::npos;
        bool inAng = false;
        for (size_t k = 0; k < url.size(); ++k) {
            wchar_t c = url[k];
            if (c == L'<') inAng = true;
            else if (c == L'>') inAng = false;
            else if (!inAng && (c == L' ' || c == L'\t')) { sp = k; break; }
        }
        if (sp != std::wstring::npos) urlOnly = url.substr(0, sp);
        if (urlOnly.size() >= 2 && urlOnly.front() == L'<' && urlOnly.back() == L'>') {
            urlOnly = urlOnly.substr(1, urlOnly.size() - 2);
        }
        std::wstring label = src.substr(labelStart, labelEnd - labelStart);

        if (isImage) {
            Inline it;
            it.kind = InlineKind::Image;
            it.text = label;
            it.imagePath = ResolveImagePath(WideToUtf8(urlOnly), base);
            out.push_back(std::move(it));
        } else {
            std::vector<Inline> innerOut = ParseInner(label, style | S_LINK, base);
            for (auto& x : innerOut) {
                x.linkUrl = urlOnly;
                out.push_back(std::move(x));
            }
        }
        pos = j + 1;
        return true;
    }

    bool tryAutolink() {
        if (src[pos] != L'<') return false;
        size_t close = src.find(L'>', pos + 1);
        if (close == std::wstring::npos) return false;
        std::wstring inner = src.substr(pos + 1, close - pos - 1);
        // Plausible URL: contains "://" and no spaces
        if (inner.find(L"://") == std::wstring::npos) return false;
        for (wchar_t c : inner) if (c == L' ' || c == L'\t' || c == L'\n') return false;
        Inline it;
        it.kind = InlineKind::Text;
        it.text = inner;
        it.style = style | S_LINK;
        it.linkUrl = inner;
        out.push_back(std::move(it));
        pos = close + 1;
        return true;
    }

    // Parse an inline HTML tag. We only care about <img> and <br>; other
    // tags are silently consumed so they don't show as raw text. Quoted
    // attribute values may contain '>' so we have to scan with quote
    // awareness.
    bool tryHtmlTag() {
        if (src[pos] != L'<') return false;
        size_t end = pos + 1;
        bool   inQuote = false;
        wchar_t qch = 0;
        while (end < src.size()) {
            wchar_t c = src[end];
            if (inQuote) {
                if (c == qch) inQuote = false;
            } else {
                if (c == L'"' || c == L'\'') { inQuote = true; qch = c; }
                else if (c == L'>') break;
                else if (c == L'<') return false;
                else if (c == L'\n') {
                    // Don't span line breaks for tags inside paragraphs
                    return false;
                }
            }
            ++end;
        }
        if (end >= src.size()) return false;

        std::wstring tag(src.begin() + pos + 1, src.begin() + end);
        size_t i = 0;
        if (i < tag.size() && tag[i] == L'/') ++i;
        size_t nameStart = i;
        while (i < tag.size() && (iswalnum(tag[i]) || tag[i] == L'-')) ++i;
        if (i == nameStart) return false;
        std::wstring name(tag.begin() + nameStart, tag.begin() + i);
        for (auto& c : name) c = (wchar_t)towlower(c);

        if (name == L"img") {
            std::wstring srcAttr    = ParseHtmlAttr(tag, L"src");
            std::wstring altAttr    = ParseHtmlAttr(tag, L"alt");
            std::wstring widthAttr  = ParseHtmlAttr(tag, L"width");
            std::wstring heightAttr = ParseHtmlAttr(tag, L"height");
            if (!srcAttr.empty()) {
                Inline it;
                it.kind = InlineKind::Image;
                it.text = altAttr;
                int n8 = WideCharToMultiByte(CP_UTF8, 0, srcAttr.data(),
                                              (int)srcAttr.size(), nullptr, 0, nullptr, nullptr);
                std::string s8;
                if (n8 > 0) {
                    s8.resize(n8);
                    WideCharToMultiByte(CP_UTF8, 0, srcAttr.data(),
                                        (int)srcAttr.size(), s8.data(), n8, nullptr, nullptr);
                }
                it.imagePath = ResolveImagePath(s8, base);

                // Parse width / height. "100%" -> percent flag set.
                auto parseSize = [](const std::wstring& s, int* outVal, bool* outPct) {
                    *outVal = 0; *outPct = false;
                    size_t i = 0;
                    while (i < s.size() && iswspace(s[i])) ++i;
                    int v = 0; bool any = false;
                    while (i < s.size() && s[i] >= L'0' && s[i] <= L'9') {
                        v = v * 10 + (s[i] - L'0'); any = true; ++i;
                    }
                    if (!any) return;
                    *outVal = v;
                    while (i < s.size() && iswspace(s[i])) ++i;
                    if (i < s.size() && s[i] == L'%') *outPct = true;
                };
                parseSize(widthAttr,  &it.imageReqW, &it.imageWPct);
                parseSize(heightAttr, &it.imageReqH, &it.imageHPct);

                out.push_back(std::move(it));
            }
            pos = end + 1;
            return true;
        }
        if (name == L"br") {
            Inline it;
            it.kind = InlineKind::LineBreak;
            out.push_back(std::move(it));
            pos = end + 1;
            return true;
        }
        if (name == L"hr") {
            Inline it;
            it.kind = InlineKind::LineBreak;
            out.push_back(std::move(it));
            pos = end + 1;
            return true;
        }
        // Unknown tag: silently consume it (no visible text).
        pos = end + 1;
        return true;
    }

    void parse() {
        while (pos < src.size()) {
            wchar_t c = src[pos];
            if (c == L'\\' && pos + 1 < src.size()) {
                wchar_t e = src[pos + 1];
                if (e == L'\\' || e == L'`' || e == L'*' || e == L'_'
                    || e == L'{' || e == L'}' || e == L'[' || e == L']'
                    || e == L'(' || e == L')' || e == L'#' || e == L'+'
                    || e == L'-' || e == L'.' || e == L'!' || e == L'|'
                    || e == L'~' || e == L'>' || e == L'<') {
                    appendChar(e);
                    pos += 2;
                    continue;
                }
                appendChar(c);
                ++pos;
                continue;
            }
            if (c == L'`')   { if (tryInlineCode()) continue; }
            if (c == L'[' || (c == L'!' && pos + 1 < src.size() && src[pos + 1] == L'[')) {
                if (tryLinkOrImage()) continue;
            }
            if (c == L'<')   {
                // Disambiguate <html-tag> vs <http://autolink>.
                if (LooksLikeHtmlTag(src, pos)) {
                    if (tryHtmlTag()) continue;
                } else if (tryAutolink()) {
                    continue;
                }
            }
            if (c == L'*' || c == L'_') {
                // For underscore, only treat as emphasis at word boundary on open side.
                bool ok = true;
                if (c == L'_') {
                    if (pos > 0 && iswalnum(src[pos - 1])) ok = false;
                }
                if (ok) { if (tryEmphasis()) continue; }
            }
            if (c == L'~' && pos + 1 < src.size() && src[pos + 1] == L'~') {
                if (tryStrike()) continue;
            }
            appendChar(c);
            ++pos;
        }
    }

    static std::vector<Inline> ParseInner(const std::wstring& s, uint32_t outerStyle, const std::wstring& base) {
        InlineParser p(s, base);
        p.style = outerStyle;
        p.parse();
        return std::move(p.out);
    }
};

// Parse an inline-text line (with hard breaks already encoded).
std::vector<Inline> ParseInlineLine(const std::wstring& line, const std::wstring& base) {
    InlineParser p(line, base);
    p.parse();
    return std::move(p.out);
}

// ---------- Block parser ---------------------------------------------------

struct ParseCtx {
    std::wstring base;
    Document     doc;
};

void FinishParagraph(ParseCtx& ctx, std::string& accum) {
    if (accum.empty()) return;
    // Trim a single trailing newline
    while (!accum.empty() && (accum.back() == '\n' || accum.back() == ' ' || accum.back() == '\t'))
        accum.pop_back();
    if (accum.empty()) return;

    // Convert to wide
    std::wstring w = Utf8ToWide(accum);
    accum.clear();

    // Translate hard line breaks: two trailing spaces or backslash before \n.
    // We've joined with '\n' between source lines; iterate to break paragraphs
    // into Inline segments separated by LineBreak.
    Block b;
    b.type = BType::Paragraph;
    std::wstring buf;
    for (size_t i = 0; i < w.size(); ++i) {
        if (w[i] == L'\n') {
            // Hard break if buf ends with two spaces or '\\'.
            bool hard = false;
            if (buf.size() >= 2 && buf[buf.size()-1] == L' ' && buf[buf.size()-2] == L' ') {
                hard = true;
                while (!buf.empty() && buf.back() == L' ') buf.pop_back();
            } else if (!buf.empty() && buf.back() == L'\\') {
                hard = true;
                buf.pop_back();
            }
            // Parse buf segment, then add break / space
            auto seg = ParseInlineLine(buf, ctx.base);
            for (auto& x : seg) b.inlines.push_back(std::move(x));
            buf.clear();
            if (hard) {
                Inline br; br.kind = InlineKind::LineBreak; b.inlines.push_back(std::move(br));
            } else {
                Inline sp; sp.kind = InlineKind::Text; sp.text = L" ";
                b.inlines.push_back(std::move(sp));
            }
        } else {
            buf.push_back(w[i]);
        }
    }
    if (!buf.empty()) {
        auto seg = ParseInlineLine(buf, ctx.base);
        for (auto& x : seg) b.inlines.push_back(std::move(x));
    }

    // Special-case: if paragraph is exactly one image inline, promote to ImageBlock.
    int imgCount = 0;
    int textNonSpace = 0;
    int total = 0;
    Inline imgInline;
    for (const auto& it : b.inlines) {
        ++total;
        if (it.kind == InlineKind::Image) { ++imgCount; imgInline = it; }
        else if (it.kind == InlineKind::Text) {
            for (wchar_t c : it.text) if (!iswspace(c)) { ++textNonSpace; break; }
        }
    }
    if (imgCount == 1 && textNonSpace == 0) {
        Block img;
        img.type = BType::ImageBlock;
        img.imagePath = imgInline.imagePath;
        img.imageAlt  = imgInline.text;
        img.imageReqW = imgInline.imageReqW;
        img.imageReqH = imgInline.imageReqH;
        img.imageWPct = imgInline.imageWPct;
        img.imageHPct = imgInline.imageHPct;
        ctx.doc.blocks.push_back(std::move(img));
        return;
    }

    ctx.doc.blocks.push_back(std::move(b));
}

// Emit a heading block from a single line of text (already inline-processable).
void EmitHeading(ParseCtx& ctx, const std::wstring& text, int level) {
    Block b;
    b.type = BType::Heading;
    b.headingLevel = level;
    b.inlines = ParseInlineLine(text, ctx.base);
    ctx.doc.blocks.push_back(std::move(b));
}

// Emit a code block.
void EmitCodeBlock(ParseCtx& ctx, const std::wstring& code, const std::wstring& lang) {
    Block b;
    b.type = BType::CodeBlock;
    b.codeText = code;
    b.codeLang = lang;
    ctx.doc.blocks.push_back(std::move(b));
}

void EmitHorizontalRule(ParseCtx& ctx) {
    Block b;
    b.type = BType::HorizontalRule;
    ctx.doc.blocks.push_back(std::move(b));
}

void EmitBlockQuote(ParseCtx& ctx, const std::wstring& joinedText) {
    Block b;
    b.type = BType::BlockQuote;
    // The blockquote is treated as a paragraph for inline purposes.
    std::wstring src = joinedText;
    // Hard break logic similar to paragraph
    std::wstring buf;
    for (size_t i = 0; i < src.size(); ++i) {
        if (src[i] == L'\n') {
            bool hard = false;
            if (buf.size() >= 2 && buf[buf.size()-1] == L' ' && buf[buf.size()-2] == L' ') {
                hard = true;
                while (!buf.empty() && buf.back() == L' ') buf.pop_back();
            } else if (!buf.empty() && buf.back() == L'\\') {
                hard = true;
                buf.pop_back();
            }
            auto seg = ParseInlineLine(buf, ctx.base);
            for (auto& x : seg) b.inlines.push_back(std::move(x));
            buf.clear();
            if (hard) {
                Inline br; br.kind = InlineKind::LineBreak; b.inlines.push_back(std::move(br));
            } else {
                Inline sp; sp.kind = InlineKind::Text; sp.text = L" ";
                b.inlines.push_back(std::move(sp));
            }
        } else buf.push_back(src[i]);
    }
    if (!buf.empty()) {
        auto seg = ParseInlineLine(buf, ctx.base);
        for (auto& x : seg) b.inlines.push_back(std::move(x));
    }
    ctx.doc.blocks.push_back(std::move(b));
}

} // namespace

// ---------- Public Parse ---------------------------------------------------

Document Parse(const std::string& utf8text, const std::wstring& basePath) {
    ParseCtx ctx;
    ctx.base = basePath;

    std::vector<std::string> lines = SplitLines(utf8text);

    std::string paraAccum;          // holds current paragraph being built
    std::string quoteAccum;         // holds current blockquote text
    bool inQuote = false;

    auto flushParagraph = [&]() { FinishParagraph(ctx, paraAccum); };
    auto flushQuote = [&]() {
        if (!inQuote) return;
        EmitBlockQuote(ctx, Utf8ToWide(quoteAccum));
        quoteAccum.clear();
        inQuote = false;
    };

    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];

        // 1) Fenced code block
        {
            int    fenceLen = 0;
            std::string info;
            char m = FenceChar(line, &fenceLen, &info);
            if (m) {
                flushParagraph();
                flushQuote();
                std::string code;
                ++i;
                for (; i < lines.size(); ++i) {
                    if (IsClosingFence(lines[i], m, fenceLen)) break;
                    if (!code.empty()) code.push_back('\n');
                    code += lines[i];
                }
                EmitCodeBlock(ctx, Utf8ToWide(code), Utf8ToWide(info));
                continue;
            }
        }

        // 2) Blank line
        if (IsBlank(line)) {
            flushParagraph();
            flushQuote();
            continue;
        }

        // 3) ATX heading
        size_t hStart = 0;
        int hLvl = AtxHeadingLevel(line, &hStart);
        if (hLvl > 0) {
            flushParagraph();
            flushQuote();
            std::string content = StripAtxTail(line.substr(hStart));
            EmitHeading(ctx, Utf8ToWide(content), hLvl);
            continue;
        }

        // 4) Setext heading: previous line text + this line === or ---
        if (!paraAccum.empty()) {
            int s = SetextUnderline(line);
            if (s) {
                // Convert paraAccum (single paragraph) to heading.
                std::string h = paraAccum;
                paraAccum.clear();
                while (!h.empty() && (h.back() == '\n' || h.back() == ' ' || h.back() == '\t'))
                    h.pop_back();
                EmitHeading(ctx, Utf8ToWide(h), s);
                continue;
            }
        }

        // 5) Horizontal rule
        if (IsHorizontalRule(line)) {
            flushParagraph();
            flushQuote();
            EmitHorizontalRule(ctx);
            continue;
        }

        // 6) Blockquote (collapse consecutive lines)
        size_t qStart = 0;
        if (IsBlockquote(line, &qStart)) {
            flushParagraph();
            if (!inQuote) { inQuote = true; quoteAccum.clear(); }
            else quoteAccum.push_back('\n');
            quoteAccum += line.substr(qStart);
            continue;
        } else {
            flushQuote();
        }

        // 7) Tables: header row + alignment row
        if (LooksLikeTableRow(line) && i + 1 < lines.size()
            && LooksLikeTableRow(lines[i+1]))
        {
            std::vector<std::string> hdr = SplitTableRow(line);
            std::vector<std::string> align = SplitTableRow(lines[i+1]);
            std::vector<int> aligns;
            if (!hdr.empty() && hdr.size() == align.size() && ParseAlignRow(align, aligns)) {
                flushParagraph();
                Block tb;
                tb.type = BType::Table;
                tb.tableAligns = aligns;
                // Header
                for (auto& c : hdr) {
                    TableCell cell;
                    cell.inlines = ParseInlineLine(Utf8ToWide(c), ctx.base);
                    tb.tableHeader.cells.push_back(std::move(cell));
                }
                // Body
                size_t k = i + 2;
                while (k < lines.size() && LooksLikeTableRow(lines[k]) && !IsBlank(lines[k])) {
                    std::vector<std::string> row = SplitTableRow(lines[k]);
                    TableRow r;
                    for (size_t c = 0; c < hdr.size(); ++c) {
                        TableCell cell;
                        std::string raw = (c < row.size()) ? row[c] : "";
                        cell.inlines = ParseInlineLine(Utf8ToWide(raw), ctx.base);
                        r.cells.push_back(std::move(cell));
                    }
                    tb.tableBody.push_back(std::move(r));
                    ++k;
                }
                ctx.doc.blocks.push_back(std::move(tb));
                i = k - 1;
                continue;
            }
        }

        // 8) Lists
        int  uIndent = 0;
        int  uContent = UnorderedBullet(line, &uIndent);
        int  oNum = 0, oIndent = 0;
        int  oContent = OrderedBullet(line, &oIndent, &oNum);
        if (uContent > 0 || oContent > 0) {
            flushParagraph();
            bool ordered = (oContent > 0);
            int  startNum = ordered ? oNum : 1;
            Block b;
            b.type = ordered ? BType::OrderedList : BType::UnorderedList;
            b.listStartIndex = startNum;

            size_t k = i;
            while (k < lines.size()) {
                int u2i = 0, u2c = UnorderedBullet(lines[k], &u2i);
                int o2n = 0, o2i = 0, o2c = OrderedBullet(lines[k], &o2i, &o2n);
                if (ordered) {
                    if (o2c <= 0) {
                        // Continuation only if line is indented and not blank.
                        if (IsBlank(lines[k]) || LeadingSpaces(lines[k]) < 2) break;
                        if (b.listItems.empty()) break;
                        // append to last item
                        std::string s = lines[k];
                        size_t off = 0;
                        while (off < s.size() && (s[off] == ' ' || s[off] == '\t')) ++off;
                        std::wstring extra = L" " + Utf8ToWide(s.substr(off));
                        // Re-tokenize the list item's inlines: simplest -> add raw text inline.
                        std::vector<Inline> add = ParseInlineLine(extra, ctx.base);
                        for (auto& x : add) b.listItems.back().inlines.push_back(std::move(x));
                        ++k;
                        continue;
                    }
                    ListItem li;
                    std::string content = lines[k].substr(o2c);
                    bool checked = false; size_t ta = 0;
                    std::wstring wc = Utf8ToWide(content);
                    if (TaskMarker(content, &checked, &ta)) {
                        li.taskMarker = true;
                        li.taskChecked = checked;
                        wc = Utf8ToWide(content.substr(ta));
                    }
                    li.inlines = ParseInlineLine(wc, ctx.base);
                    b.listItems.push_back(std::move(li));
                    ++k;
                    continue;
                } else {
                    if (u2c <= 0) {
                        if (IsBlank(lines[k]) || LeadingSpaces(lines[k]) < 2) break;
                        if (b.listItems.empty()) break;
                        std::string s = lines[k];
                        size_t off = 0;
                        while (off < s.size() && (s[off] == ' ' || s[off] == '\t')) ++off;
                        std::wstring extra = L" " + Utf8ToWide(s.substr(off));
                        std::vector<Inline> add = ParseInlineLine(extra, ctx.base);
                        for (auto& x : add) b.listItems.back().inlines.push_back(std::move(x));
                        ++k;
                        continue;
                    }
                    ListItem li;
                    std::string content = lines[k].substr(u2c);
                    bool checked = false; size_t ta = 0;
                    std::wstring wc = Utf8ToWide(content);
                    if (TaskMarker(content, &checked, &ta)) {
                        li.taskMarker = true;
                        li.taskChecked = checked;
                        wc = Utf8ToWide(content.substr(ta));
                    }
                    li.inlines = ParseInlineLine(wc, ctx.base);
                    b.listItems.push_back(std::move(li));
                    ++k;
                    continue;
                }
            }
            ctx.doc.blocks.push_back(std::move(b));
            i = k - 1;
            continue;
        }

        // 9) Default: accumulate into paragraph
        if (!paraAccum.empty()) paraAccum.push_back('\n');
        paraAccum += line;
    }
    flushParagraph();
    flushQuote();

    return std::move(ctx.doc);
}

} // namespace md
