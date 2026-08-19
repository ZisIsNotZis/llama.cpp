#include "markup.h"

#include "md4c.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace markup {

// ---------------------------------------------------------------------------
// md4c SAX -> styled lines
// ---------------------------------------------------------------------------

struct span {
    uint16_t style;
    uint8_t  color;
};

struct md_state {
    std::vector<line> out;
    line              cur;
    std::vector<span> stk;

    bool        in_code    = false;
    std::string code_buf;
    std::string code_lang;

    void push(uint16_t s, uint8_t c) {
        span top{0, C_DEFAULT};
        if (!stk.empty()) {
            top = stk.back();
        }
        top.style |= s;
        if (c != C_DEFAULT) {
            top.color = c;
        }
        stk.push_back(top);
    }

    void pop() {
        if (!stk.empty()) {
            stk.pop_back();
        }
    }

    span top() const {
        return stk.empty() ? span{0, C_DEFAULT} : stk.back();
    }

    void newline() {
        if (!cur.empty()) {
            out.push_back(std::move(cur));
            cur.clear();
        }
    }
};

static int enter_block(MD_BLOCKTYPE type, void * detail, void * ud) {
    auto & st = *(md_state *) ud;
    switch (type) {
    case MD_BLOCK_H: {
        const MD_BLOCK_H_DETAIL * d = (const MD_BLOCK_H_DETAIL *) detail;
        uint16_t s = S_HEADING;
        if (d->level <= 2) {
            s |= S_BOLD;
        }
        st.push(s, C_DEFAULT);
        break;
    }
    case MD_BLOCK_CODE: {
        const MD_BLOCK_CODE_DETAIL * d = (const MD_BLOCK_CODE_DETAIL *) detail;
        st.in_code   = true;
        st.code_lang.assign(d->info.text, d->info.size);
        st.code_buf.clear();
        break;
    }
    case MD_BLOCK_QUOTE:
        st.push(S_QUOTE, C_DEFAULT);
        break;
    case MD_BLOCK_HR: {
        line l;
        l.push_back({"---", S_DIM, C_DEFAULT});
        st.out.push_back(std::move(l));
        break;
    }
    default:
        break;
    }
    return 0;
}

static int leave_block(MD_BLOCKTYPE type, void * detail, void * ud) {
    (void) detail;
    auto & st = *(md_state *) ud;
    switch (type) {
    case MD_BLOCK_H:
        st.newline();
        st.pop(); // heading style
        st.out.push_back(line{}); // blank separator after a heading
        break;
    case MD_BLOCK_P:
    case MD_BLOCK_LI:
        st.newline();
        break;
    case MD_BLOCK_QUOTE:
        st.newline();
        st.pop();
        break;
    case MD_BLOCK_CODE: {
        st.in_code = false;
        if (!st.code_buf.empty()) {
            auto lines = highlight_code(st.code_lang, st.code_buf);
            if (lines.empty()) {
                lines = plain_code(st.code_buf);
            }
            for (auto & l : lines) {
                st.out.push_back(std::move(l));
            }
        }
        st.code_buf.clear();
        st.code_lang.clear();
        st.out.push_back(line{}); // blank separator after a code block
        break;
    }
    default:
        break;
    }
    return 0;
}

static int enter_span(MD_SPANTYPE type, void * detail, void * ud) {
    auto & st = *(md_state *) ud;
    (void) detail;
    switch (type) {
    case MD_SPAN_EM:
        st.push(S_ITALIC, C_DEFAULT);
        break;
    case MD_SPAN_STRONG:
        st.push(S_BOLD, C_DEFAULT);
        break;
    case MD_SPAN_CODE:
        st.push(S_CODE, C_DEFAULT);
        break;
    case MD_SPAN_DEL:
        st.push(S_DIM, C_DEFAULT);
        break;
    default:
        st.push(0, C_DEFAULT); // keep the style stack balanced
        break;
    }
    return 0;
}

static int leave_span(MD_SPANTYPE type, void * detail, void * ud) {
    auto & st = *(md_state *) ud;
    (void) type;
    (void) detail;
    st.pop();
    return 0;
}

static int text_cb(MD_TEXTTYPE type, const MD_CHAR * s, MD_SIZE n, void * ud) {
    auto & st = *(md_state *) ud;
    if (st.in_code) {
        if (type == MD_TEXT_SOFTBR || type == MD_TEXT_BR) {
            st.code_buf.push_back('\n');
        } else {
            st.code_buf.append(s, n);
        }
        return 0;
    }
    switch (type) {
    case MD_TEXT_SOFTBR:
    case MD_TEXT_BR:
        st.newline();
        return 0;
    default: {
        const span t = st.top();
        if (!st.cur.empty() && st.cur.back().style == t.style && st.cur.back().color == t.color) {
            st.cur.back().text.append(s, n);
        } else {
            run r;
            r.text  = std::string(s, n);
            r.style = t.style;
            r.color = t.color;
            st.cur.push_back(std::move(r));
        }
        return 0;
    }
    }
}

static void debug_log(const char * msg, void * ud) {
    (void) msg;
    (void) ud;
}

std::vector<line> parse(const std::string & md) {
    md_state st;
    MD_PARSER p;
    std::memset(&p, 0, sizeof(p));
    p.flags       = MD_DIALECT_GITHUB | MD_FLAG_NOHTML;
    p.enter_block = enter_block;
    p.leave_block = leave_block;
    p.enter_span  = enter_span;
    p.leave_span  = leave_span;
    p.text        = text_cb;
    p.debug_log   = debug_log;
    if (md_parse(md.data(), (MD_SIZE) md.size(), &p, &st) != 0) {
        st.out.clear(); // parse aborted; treat as plain text below
    }
    st.newline(); // flush the last line
    if (st.out.empty()) {
        return plain_code(md); // plain-text fallback
    }
    return std::move(st.out);
}

// ---------------------------------------------------------------------------
// lenient syntax lexer
// ---------------------------------------------------------------------------

struct lang_conf {
    const char *                 name;
    const char * const *         kw;
    int                          n_kw;
    bool hash_comment;   // '#' line comment (python, bash, yaml)
    bool slash_comment;  // '//' and '/* */' (c/cpp, java, json5)
    bool dash_comment;   // '--' line comment (sql)
    bool dollar_var;     // $var (bash)
    bool triple_quote;   // """ ''' (python)
    bool cpp;            // '#' at line start = preprocessor macro
    bool yaml_key;       // 'key:' at line start
    bool single_quote;   // '...' strings
    bool double_quote;   // "..." strings
};

static bool has_kw(const lang_conf & cf, const char * w, size_t n) {
    for (int i = 0; i < cf.n_kw; i++) {
        if (strlen(cf.kw[i]) == n && strncmp(cf.kw[i], w, n) == 0) {
            return true;
        }
    }
    return false;
}

static const char * const PY_KEYWORDS[] = {
    "def", "class", "if", "elif", "else", "for", "while", "return", "import", "from",
    "as", "with", "lambda", "pass", "break", "continue", "try", "except", "finally",
    "raise", "yield", "global", "nonlocal", "assert", "del", "in", "is", "not", "and",
    "or", "None", "True", "False", "async", "await",
};

static const char * const SH_KEYWORDS[] = {
    "if", "then", "else", "elif", "fi", "for", "while", "until", "do", "done",
    "case", "esac", "function", "return", "local", "export", "source", "read",
    "echo", "exit", "trap", "set", "unset", "declare", "shift", "cd", "break",
    "continue", "select", "in",
};

static const char * const C_KEYWORDS[] = {
    "auto", "break", "case", "char", "const", "continue", "default", "do", "double",
    "else", "enum", "extern", "float", "for", "goto", "if", "inline", "int", "long",
    "register", "return", "short", "signed", "sizeof", "static", "struct", "switch",
    "typedef", "union", "unsigned", "void", "volatile", "while", "bool", "true",
    "false", "nullptr", "class", "namespace", "template", "typename", "public",
    "private", "protected", "virtual", "override", "new", "delete", "this", "using",
};

static const char * const JAVA_KEYWORDS[] = {
    "abstract", "boolean", "break", "byte", "case", "catch", "char", "class",
    "const", "continue", "default", "do", "double", "else", "enum", "extends",
    "final", "finally", "float", "for", "goto", "if", "implements", "import",
    "instanceof", "int", "interface", "long", "native", "new", "package", "private",
    "protected", "public", "return", "short", "static", "strictfp", "super",
    "switch", "synchronized", "this", "throw", "throws", "transient", "try", "void",
    "volatile", "while", "true", "false", "null",
};

static const char * const SQL_KEYWORDS[] = {
    "select", "from", "where", "insert", "into", "values", "update", "set", "delete",
    "create", "table", "alter", "drop", "join", "left", "right", "inner", "outer",
    "full", "on", "group", "by", "order", "having", "limit", "offset", "and", "or",
    "not", "null", "as", "in", "is", "like", "between", "distinct", "union", "all",
    "case", "when", "then", "else", "end", "primary", "key", "foreign", "references",
    "index", "view", "procedure", "function", "trigger", "begin", "commit", "rollback",
    "if", "exists", "unique", "default", "with",
};

static const char * const JSON_KEYWORDS[] = {"true", "false", "null"};

static const char * const YAML_KEYWORDS[] = {"true", "false", "null", "yes", "no", "on", "off"};

static const lang_conf LANG_PY   = {"python", PY_KEYWORDS,   (int) (sizeof(PY_KEYWORDS) / sizeof(*PY_KEYWORDS)),   true,  false, false, false, true,  false, false, true,  true};
static const lang_conf LANG_SH   = {"bash",   SH_KEYWORDS,   (int) (sizeof(SH_KEYWORDS) / sizeof(*SH_KEYWORDS)),   true,  false, false, true,  false, false, false, true,  true};
static const lang_conf LANG_C    = {"c",      C_KEYWORDS,    (int) (sizeof(C_KEYWORDS) / sizeof(*C_KEYWORDS)),     false, true,  false, false, false, true,  false, true,  true};
static const lang_conf LANG_JAVA = {"java",   JAVA_KEYWORDS, (int) (sizeof(JAVA_KEYWORDS) / sizeof(*JAVA_KEYWORDS)), false, true, false, false, false, false, false, true, true};
static const lang_conf LANG_SQL  = {"sql",    SQL_KEYWORDS,  (int) (sizeof(SQL_KEYWORDS) / sizeof(*SQL_KEYWORDS)), false, false, true,  false, false, false, false, true,  true};
static const lang_conf LANG_JSON = {"json",   JSON_KEYWORDS, (int) (sizeof(JSON_KEYWORDS) / sizeof(*JSON_KEYWORDS)), false, false, false, false, false, false, false, true, true};
static const lang_conf LANG_YAML = {"yaml",   YAML_KEYWORDS, (int) (sizeof(YAML_KEYWORDS) / sizeof(*YAML_KEYWORDS)), true, false, false, false, false, false, true,  true, true};

static const lang_conf * lang_for(const std::string & lang) {
    // normalize to lowercase
    std::string l = lang;
    for (auto & c : l) {
        c = (char) std::tolower((unsigned char) c);
    }
    // strip common qualifiers like "python3", "c++17", "bash"
    std::string base;
    for (size_t i = 0; i < l.size(); i++) {
        if (std::isalpha((unsigned char) l[i]) || l[i] == '+') {
            base.push_back(l[i]);
        } else {
            break;
        }
    }
    if (base == "py" || base == "python" || base == "python3") return &LANG_PY;
    if (base == "sh" || base == "bash" || base == "zsh" || base == "shell") return &LANG_SH;
    if (base == "c" || base == "cpp" || base == "c++" || base == "cxx" || base == "h" || base == "hpp") return &LANG_C;
    if (base == "java") return &LANG_JAVA;
    if (base == "sql") return &LANG_SQL;
    if (base == "json" || base == "json5") return &LANG_JSON;
    if (base == "yaml" || base == "yml") return &LANG_YAML;
    return nullptr;
}

// tokenize `code` into colored lines. Returns false if the block looks
// malformed (caller falls back to plain text).
static bool tokenize(const lang_conf & cf, const std::string & code, std::vector<line> & out) {
    const size_t n = code.size();
    if (n > 1 << 20) {
        return false; // unreasonably large block; do not highlight
    }

    line cur;
    auto add = [&](size_t start, size_t len, uint8_t color) {
        if (len == 0) {
            return;
        }
        run r;
        r.text  = code.substr(start, len);
        r.style = S_CODE;
        r.color = color;
        if (!cur.empty() && cur.back().style == r.style && cur.back().color == r.color) {
            cur.back().text += r.text;
        } else {
            cur.push_back(std::move(r));
        }
    };
    auto flush = [&]() {
        out.push_back(std::move(cur));
        cur = line();
    };

    size_t i = 0;
    while (i < n) {
        const char c = code[i];
        if (c == '\n') {
            flush();
            i++;
            continue;
        }
        if (std::isspace((unsigned char) c)) {
            size_t j = i;
            while (j < n && std::isspace((unsigned char) code[j])) {
                j++;
            }
            add(i, j - i, C_DEFAULT);
            i = j;
            continue;
        }

        // preprocessor macro (c/cpp): '#' at line start
        if (cf.cpp && c == '#') {
            bool line_start = (i == 0) || code[i - 1] == '\n';
            if (line_start) {
                size_t j = i;
                while (j < n && code[j] != '\n') {
                    j++;
                }
                add(i, j - i, C_META);
                i = j;
                continue;
            }
        }

        // line comments
        if (cf.hash_comment && c == '#') {
            size_t j = i;
            while (j < n && code[j] != '\n') {
                j++;
            }
            add(i, j - i, C_CMT);
            i = j;
            continue;
        }
        if (cf.slash_comment && c == '/' && i + 1 < n && code[i + 1] == '/') {
            size_t j = i;
            while (j < n && code[j] != '\n') {
                j++;
            }
            add(i, j - i, C_CMT);
            i = j;
            continue;
        }
        if (cf.dash_comment && c == '-' && i + 1 < n && code[i + 1] == '-') {
            size_t j = i;
            while (j < n && code[j] != '\n') {
                j++;
            }
            add(i, j - i, C_CMT);
            i = j;
            continue;
        }

        // block comments
        if (cf.slash_comment && c == '/' && i + 1 < n && code[i + 1] == '*') {
            const size_t end = code.find("*/", i + 2);
            if (end == std::string::npos) {
                return false; // unterminated block comment -> malformed
            }
            add(i, end + 2 - i, C_CMT);
            i = end + 2;
            continue;
        }

        // triple-quoted strings (python)
        if (cf.triple_quote && (c == '"' || c == '\'') && i + 2 < n &&
            code[i + 1] == c && code[i + 2] == c) {
            const std::string delim = std::string(3, c);
            const size_t end = code.find(delim, i + 3);
            if (end == std::string::npos) {
                // unclosed docstring -> color to end, do not corrupt
                add(i, n - i, C_STR);
                i = n;
            } else {
                add(i, end + 3 - i, C_STR);
                i = end + 3;
            }
            continue;
        }

        // single-line strings
        if ((c == '"' && cf.double_quote) || (c == '\'' && cf.single_quote)) {
            const char q = c;
            size_t j = i + 1;
            bool closed = false;
            while (j < n) {
                if (code[j] == '\\' && j + 1 < n) {
                    j += 2;
                    continue;
                }
                if (code[j] == q) {
                    closed = true;
                    j++;
                    break;
                }
                if (code[j] == '\n' && !cf.triple_quote) {
                    break; // single-line language; do not span lines
                }
                j++;
            }
            if (!closed) {
                return false; // unterminated string -> malformed block
            }
            add(i, j - i, C_STR);
            i = j;
            continue;
        }

        // dollar variables (bash)
        if (cf.dollar_var && c == '$' && i + 1 < n) {
            size_t j = i + 1;
            if (code[j] == '{') {
                const size_t end = code.find('}', j);
                if (end == std::string::npos) {
                    return false;
                }
                j = end + 1;
            } else {
                while (j < n && (std::isalnum((unsigned char) code[j]) || code[j] == '_')) {
                    j++;
                }
            }
            add(i, j - i, C_META);
            i = j;
            continue;
        }

        // numbers
        if (std::isdigit((unsigned char) c) ||
            (c == '.' && i + 1 < n && std::isdigit((unsigned char) code[i + 1]))) {
            size_t j = i;
            if (c == '0' && i + 1 < n && (code[i + 1] == 'x' || code[i + 1] == 'X')) {
                j = i + 2;
                while (j < n && std::isxdigit((unsigned char) code[j])) {
                    j++;
                }
            } else {
                while (j < n && (std::isalnum((unsigned char) code[j]) || code[j] == '.' || code[j] == '_')) {
                    j++;
                }
            }
            add(i, j - i, C_NUM);
            i = j;
            continue;
        }

        // identifiers / keywords
        if (std::isalpha((unsigned char) c) || c == '_') {
            size_t j = i;
            while (j < n && (std::isalnum((unsigned char) code[j]) || code[j] == '_')) {
                j++;
            }
            const size_t len = j - i;
            uint8_t color = has_kw(cf, code.c_str() + i, len) ? C_KW : C_DEFAULT;
            // yaml key at line start: identifier followed by ':'
            if (cf.yaml_key && color == C_DEFAULT) {
                size_t k = j;
                while (k < n && code[k] == ' ') {
                    k++;
                }
                if (k < n && code[k] == ':') {
                    color = C_META;
                }
            }
            add(i, len, color);
            i = j;
            continue;
        }

        // symbols / operators
        {
            size_t j = i;
            while (j < n) {
                const char d = code[j];
                if (d == '\n' || std::isspace((unsigned char) d) ||
                    std::isalnum((unsigned char) d) || d == '_' ||
                    d == '"' || d == '\'' || d == '#') {
                    break;
                }
                if (cf.slash_comment && d == '/' && j + 1 < n && (code[j + 1] == '/' || code[j + 1] == '*')) {
                    break;
                }
                j++;
            }
            if (j == i) {
                j = i + 1; // safety; should not happen
            }
            add(i, j - i, C_SYM);
            i = j;
        }
    }
    if (!cur.empty()) {
        flush();
    }
    return true;
}

std::vector<line> highlight_code(const std::string & lang, const std::string & code) {
    const lang_conf * cf = lang_for(lang);
    if (!cf) {
        return std::vector<line>(); // unknown language -> no highlight, plain fallback
    }
    std::vector<line> out;
    if (!tokenize(*cf, code, out)) {
        return std::vector<line>();
    }
    return out;
}

std::vector<line> plain_code(const std::string & code) {
    std::vector<line> out;
    line cur;
    size_t start = 0;
    for (size_t i = 0; i <= code.size(); i++) {
        if (i == code.size() || code[i] == '\n') {
            if (i > start || !cur.empty()) {
                run r;
                r.text  = code.substr(start, i - start);
                r.style = S_CODE;
                r.color = C_DEFAULT;
                if (!cur.empty() && cur.back().style == r.style && cur.back().color == r.color) {
                    cur.back().text += r.text;
                } else {
                    cur.push_back(std::move(r));
                }
            }
            out.push_back(std::move(cur));
            cur = line();
            start = i + 1;
        }
    }
    return out;
}

} // namespace markup
