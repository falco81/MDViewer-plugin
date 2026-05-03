# MDViewer — Markdown Lister Plugin for Total Commander

A Total Commander **Lister plugin** (`.wlx` / `.wlx64`) that renders Markdown
files (`.md`, `.markdown`, `.mkd`, `.mdown`, `.mdx`) as fully formatted
output when you press **F3** on them.

**Pure native Win32**: no Internet Explorer, no embedded browser, no HTML
engine, no JavaScript. The plugin parses Markdown into an AST and draws it
straight onto a window with GDI for text and GDI+ for images. The result
is a self-contained DLL that runs on every Windows version since 7
without any runtime dependencies beyond what ships with Windows.

---

## Distribution

This project ships as **two ZIP files**:

| ZIP                       | Contents                                                       | For                                  |
|---------------------------|----------------------------------------------------------------|--------------------------------------|
| **`MDViewer-plugin.zip`** | `pluginst.inf`, `MDViewer.wlx`, `MDViewer.wlx64`               | End users — drop into TC to install  |
| **`MDViewer-source.zip`** | All source code, build scripts, Makefile, `BUILDING.md`, README | Developers who want to compile      |

End users only need `MDViewer-plugin.zip`. Developers who want to rebuild
from source use `MDViewer-source.zip`.

---

## Features

* **Native rendering pipeline** — Markdown is tokenised into a block AST
  (`parser.cpp`), laid out into positioned draw commands
  (`renderer.cpp`), then painted onto an owner-drawn window
  (`mdviewer.cpp`). No browser is involved.
* **Headings (H1–H6)** with size hierarchy and rule under H1/H2.
* **Inline formatting**: bold, italic, inline code, strikethrough, links
  (clickable — open in default browser).
* **Code blocks** (fenced ``` and ~~~) with monospace font and tinted
  background.
* **Lists** — ordered, unordered, GitHub task lists (`- [ ]` / `- [x]`).
* **Blockquotes** with left-rail accent.
* **GFM tables** with `:---` / `:---:` / `---:` alignment.
* **Horizontal rules**.
* **Inline & block images** — JPG, PNG, GIF, BMP, TIFF loaded via GDI+,
  paths resolved relative to the file’s folder.
* **Working scrollbar** + mouse wheel, arrows, PgUp/PgDn, Home/End,
  Space/Backspace.
* **Live search overlay**: F7 / Ctrl+F opens, types narrow as you type,
  match counter shown, Prev/Next buttons, F3 / Shift+F3, Enter / Shift+Enter.
* **UTF-8 / UTF-8 BOM / UTF-16 LE / UTF-16 BE / ANSI** source files all
  detected automatically.
* **Self-contained**: statically linked C/C++ runtime, no extra DLLs.

---

## Installation (for end users)

### Easy install

1. Download `MDViewer-plugin.zip`.
2. **Inside Total Commander**, navigate to the ZIP and press Enter (or
   double-click). TC sees `pluginst.inf` at the root and offers to install.
3. Confirm the dialog. TC copies the matching DLL (32-bit or 64-bit) to a
   plugin folder and registers it.
4. The plugin auto-associates with `.md`, `.markdown`, `.mkd`, `.mdown`,
   `.mdx`. Press **F3** on any Markdown file.

### Manual install

1. Extract `MDViewer.wlx` and `MDViewer.wlx64` into, e.g.
   `%COMMANDER_PATH%\Plugins\wlx\MDViewer\`.
2. *Configuration* → *Options...* → *Edit/View* →
   *Configure internal viewer* → *WLX Plugins* → *Add*.
3. Pick `MDViewer.wlx` (TC auto-selects the right architecture if both
   files sit next to each other).
4. Detect string:
   `EXT="MD" | EXT="MARKDOWN" | EXT="MKD" | EXT="MDOWN" | EXT="MDX"`

---

## Keyboard shortcuts (inside the viewer)

| Key                    | Action                                                  |
|------------------------|---------------------------------------------------------|
| **↑ / ↓**              | Scroll one line                                         |
| **PgUp / PgDn**        | Scroll one page                                         |
| **Home / End**         | Jump to start / end                                     |
| **Space / Backspace**  | Page down / page up                                     |
| **Mouse wheel**        | Smooth scroll                                           |
| **Shift + wheel** or **Ctrl + wheel** | Zoom in / out                            |
| **Ctrl + +  /  Ctrl + −  /  Ctrl + 0** | Zoom in / out / reset                   |
| **F7 / F3**            | Open search overlay (first press); next match (when active) |
| **Shift+F7 / Shift+F3**| Previous match (when search is active)                  |
| **Ctrl+F**             | Open search overlay                                     |
| **Enter / Shift+Enter** (in search box) | Find next / previous                   |
| **Esc**                | Close search overlay; if already closed → close viewer  |
| **Click + drag**       | Select text                                             |
| **Ctrl+A**             | Select all                                              |
| **Ctrl+C**             | Copy selection (or whole document if no selection)      |
| **Ctrl+E**             | Export to PDF (via system Print dialog)                 |
| **Click on link**      | Open in default browser                                 |

---

## Building from source

See **[BUILDING.md](BUILDING.md)** for full instructions covering both
**AlmaLinux 9** (and other Linux distros) and **Windows 10/11**, with
both scripted and manual paths.

Quick reference:

| Platform     | Scripted              | Manual                                |
|--------------|-----------------------|---------------------------------------|
| AlmaLinux 9  | `./build-linux.sh`    | `dnf install … mingw* gdiplus && make all` |
| Windows 10/11 | `build-windows.bat`  | MSYS2 + `g++` per arch                |

Output of any successful build:

```
MDViewer.wlx          – 32-bit Lister DLL
MDViewer.wlx64        – 64-bit Lister DLL
MDViewer-plugin.zip   – Total Commander install bundle
```

---

## File overview

| File                | Purpose                                                       |
|---------------------|---------------------------------------------------------------|
| `parser.h/.cpp`     | Markdown → block AST (headings, lists, tables, …)             |
| `renderer.h/.cpp`   | Layout + painting via GDI (text) and GDI+ (images)            |
| `mdviewer.cpp`      | Plugin entry points, window proc, scroll, search overlay      |
| `listplug.h`        | Total Commander Lister plugin API constants                   |
| `mdviewer.def`      | DLL export list (undecorated names)                           |
| `pluginst.inf`      | Total Commander plugin installer manifest                     |
| `Makefile`          | Cross-compile recipe (GNU make)                               |
| `build-linux.sh`    | One-shot Linux dependency install + build                     |
| `build-windows.bat` | One-shot Windows dependency install + build                   |
| `BUILDING.md`       | Detailed build documentation                                  |

---

## License

Released as-is, free to use and modify.
