#pragma once

// Markdown -> styled-line model for the terminal TUI.
// Parses full cell text with md4c (MIT, vendored header) into per-line runs
// of (text, style bits, color). Fenced code blocks are tokenized by a small
// embedded lenient lexer; on a malformed construct it stops highlighting the
// block and falls back to plain text (never crashes).
// See docs/dashboard/DESIGN.md section 7.3.

#include <cstdint>
#include <string>
#include <vector>

namespace markup {

// style bits
enum : uint16_t {
    S_BOLD    = 1 << 0,
    S_ITALIC  = 1 << 1,
    S_DIM     = 1 << 2,
    S_HEADING = 1 << 3,
    S_CODE    = 1 << 4, // inline code or code block
    S_QUOTE   = 1 << 5,
};

// syntax color palette indices (0 = default)
enum : uint8_t {
    C_DEFAULT = 0,
    C_KW,     // keyword
    C_STR,    // string
    C_CMT,    // comment
    C_NUM,    // number
    C_SYM,    // operator / symbol
    C_META,   // key / macro / label
    C_TYPE,   // type name
};

struct run {
    std::string text;
    uint16_t    style = 0;
    uint8_t     color = C_DEFAULT;
};

using line = std::vector<run>;

// Parse markdown into styled lines (whole text; caller windows the tail).
// Block context is preserved because the full text is parsed up front.
std::vector<line> parse(const std::string & md);

// Tokenize a fenced code block into colored lines. Returns empty on a
// malformed construct (caller then falls back to plain code lines).
std::vector<line> highlight_code(const std::string & lang, const std::string & code);

// Plain (uncolored) code lines with the S_CODE style.
std::vector<line> plain_code(const std::string & code);

} // namespace markup
