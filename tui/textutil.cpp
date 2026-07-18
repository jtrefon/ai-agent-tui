// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "textutil.h"

#include <cstring>
#include <langinfo.h>

namespace tui {
namespace text {

namespace {
bool detect_utf8() {
    const char* cs = nl_langinfo(CODESET);
    if (!cs || (std::strcmp(cs, "UTF-8") != 0 && std::strcmp(cs, "utf8") != 0))
        return false;                       // non-UTF-8 locale: bytes would
                                            // render as garbage.
    // PuTTY advertises itself via TERM=putty* but commonly runs with a Latin-1
    // translation table, so even over a UTF-8 locale the multibyte glyphs show
    // up as raw bytes. Fall back to ASCII for any PuTTY terminal.
    const char* term = std::getenv("TERM");
    if (term && std::strncmp(term, "putty", 5) == 0) return false;
    return true;
}
} // namespace

bool glyph::utf8() {
    static const bool v = detect_utf8();
    return v;
}

const char* glyph::tool()     { return utf8() ? "\u2728" : "*"; }
const char* glyph::arrow()    { return utf8() ? "\u2192" : "->"; }
const char* glyph::middot()   { return utf8() ? "\u00b7" : "-"; }
const char* glyph::emdash()   { return utf8() ? "\u2014" : "-"; }
const char* glyph::up()       { return utf8() ? "\u2191" : "^"; }
const char* glyph::down()     { return utf8() ? "\u2193" : "v"; }
const char* glyph::block_l()  { return utf8() ? "\u2590" : "|"; }
const char* glyph::block_r()  { return utf8() ? "\u258c" : "|"; }
const char* glyph::ellipsis() { return utf8() ? "\u2026" : "..."; }

std::size_t utf8_len(const std::string& s, std::size_t i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    std::size_t n = (c < 0x80) ? 1 : (c >> 5) == 0x6 ? 2
                  : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
    // Validate continuation bytes; treat a truncated sequence as 1 byte.
    for (std::size_t k = 1; k < n; ++k)
        if (i + k >= s.size() ||
            (static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80)
            return 1;
    return n;
}

int display_cols(const std::string& s) {
    int cols = 0;
    for (std::size_t i = 0; i < s.size(); i += utf8_len(s, i)) ++cols;
    return cols;
}

std::vector<std::string> wrap(const std::string& text, int w) {
    if (w <= 0) w = 80;
    std::vector<std::string> out;
    // Sanitize: expand tabs, drop CR, strip ANSI/control bytes that would
    // otherwise be written raw to the terminal (garbage on screen), while
    // preserving valid multibyte UTF-8 (emoji/CJK) intact.
    std::string src;
    src.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '\t') { src += "    "; ++i; continue; }
        if (c == '\n') { src += '\n'; ++i; continue; }
        if (c == 0x1b) {                       // ESC: skip a CSI/simple seq
            ++i;
            if (i < text.size() && text[i] == '[') {
                ++i;
                while (i < text.size() &&
                       !(text[i] >= '@' && text[i] <= '~')) ++i;
                if (i < text.size()) ++i;      // final byte
            } else if (i < text.size()) {
                ++i;
            }
            continue;
        }
        if (c < 0x20 || c == 0x7f) { ++i; continue; }  // other control chars
        std::size_t n = utf8_len(text, i);
        if (n == 1 && c >= 0x80) { src += '?'; ++i; continue; } // bad byte
        src.append(text, i, n);
        i += n;
    }
    std::size_t start = 0;
    while (start <= src.size()) {
        std::size_t nl = src.find('\n', start);
        std::string para = (nl == std::string::npos)
                               ? src.substr(start)
                               : src.substr(start, nl - start);
        // word-wrap this paragraph
        if (para.empty()) {
            out.push_back("");
        } else {
            std::size_t p = 0;
            while (p < para.size()) {
                // Walk forward up to `w` display columns, counting whole
                // UTF-8 characters (each counted as one column) so we never
                // slice through a multibyte sequence.
                std::size_t q = p;
                int cols = 0;
                while (q < para.size() && cols < w) {
                    q += utf8_len(para, q);
                    ++cols;
                }
                if (q >= para.size()) {
                    out.push_back(para.substr(p));
                    break;
                }
                // find a space to break on within [p, q]
                std::size_t brk = para.rfind(' ', q);
                if (brk == std::string::npos || brk <= p) {
                    out.push_back(para.substr(p, q - p));  // hard split
                    p = q;
                } else {
                    out.push_back(para.substr(p, brk - p));
                    p = brk + 1;                           // skip the space
                }
            }
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

std::wstring to_wide(const std::string& s) {
    std::wstring w;
    for (std::size_t i = 0; i < s.size();) {
        std::size_t n = utf8_len(s, i);
        wchar_t cp = 0;
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (n == 1) {
            cp = c;
        } else if (n == 2) {
            cp = (c & 0x1F);
        } else if (n == 3) {
            cp = (c & 0x0F);
        } else {
            cp = (c & 0x07);
        }
        for (std::size_t k = 1; k < n; ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        w.push_back(cp);
        i += n;
    }
    return w;
}

} // namespace text
} // namespace tui
