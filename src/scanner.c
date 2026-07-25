/**
 * External scanner for the wikitext grammar.
 *
 * Responsibilities (mirroring Parsoid's Grammar.pegphp where noted):
 *  - start-of-line tokens gated on column 0: list markers, headings, hr,
 *    indent-pre, table markup, #REDIRECT (Parsoid's sol context)
 *  - prose text with lookahead stops before magic links and bare URLs
 *    (Parsoid's urltext / reUrltextLookahead)
 *  - bold/italic apostrophe runs with per-line auto-close (quote rule)
 *  - raw content of extension tags up to the matching close tag, like
 *    <script>/<style> in tree-sitter-html (maybe_extension_tag)
 */

#include "tree_sitter/parser.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum TokenType {
  LIST_MARKER,
  HEADING_OPEN,
  HEADING_CLOSE,
  HR,
  INDENT_PRE,
  TABLE_START,
  TABLE_END,
  TABLE_ROW,
  TABLE_CAPTION,
  TABLE_CELL_SOL,
  TABLE_HEADER_SOL,
  REDIRECT_KEYWORD,
  TEXT,
  BRACKET_URL,
  AUTO_URL,
  MAGIC_LINK,
  ITALIC_OPEN,
  ITALIC_CLOSE,
  BOLD_OPEN,
  BOLD_CLOSE,
  RAW_START_NAME,
  RAW_END_NAME,
  RAW_TEXT,
  RAW_SELF_CLOSE,
  HTML_TAG_NAME,
  TAG_SLASH,
  ERROR_SENTINEL,
};

// Scanner state flags (serialized).
#define F_ITALIC 1u        // inside ''...''
#define F_BOLD 2u          // inside '''...'''
#define F_BOLD_INNER 4u    // bold was opened after italic
#define F_PENDING_I 8u     // emit a zero-width italic toggle next
#define F_PENDING_B 16u    // emit a zero-width bold toggle next

typedef struct {
  uint8_t quote_flags;
  uint8_t raw_tag; // 1-based index into RAW_TAGS; 0 = none
} Scanner;

// Extension tags whose content is not wikitext. Content is lexed as one
// raw_text token; other extension tags (ref, poem, gallery, includeonly, ...)
// are "transparent" and handled by the grammar like HTML tags.
static const char *const RAW_TAGS[] = {
  "categorytree", "ce", "charinsert", "chem", "graph", "hiero", "imagemap",
  "inputbox", "maplink", "mapframe", "math", "nowiki", "pre", "score",
  "source", "syntaxhighlight", "templatedata", "timeline",
};
#define N_RAW_TAGS (sizeof(RAW_TAGS) / sizeof(RAW_TAGS[0]))

// HTML tags MediaWiki allows plus transparent extension tags whose content
// stays wikitext (Parsoid's isXMLTag / extension tag map). Anything not in
// this list or RAW_TAGS renders literally and is lexed as prose.
static const char *const HTML_TAGS[] = {
  "a", "abbr", "audio", "b", "bdi", "bdo", "big", "blockquote", "br",
  "caption", "center", "cite", "code", "data", "dd", "del", "details",
  "dfn", "div", "dl", "dt", "em", "figcaption", "figure", "font", "h1",
  "h2", "h3", "h4", "h5", "h6", "hr", "i", "img", "includeonly",
  "indicator", "ins", "kbd", "languages", "li", "link", "mark", "meta",
  "noinclude", "ol", "onlyinclude", "p", "picture", "poem", "q", "rb",
  "ref", "references", "rp", "rt", "rtc", "ruby", "s", "samp", "section",
  "small", "span", "strike", "strong", "sub", "summary", "sup", "table",
  "tbody", "td", "tfoot", "th", "thead", "time", "tr", "translate", "tt",
  "tvar", "u", "ul", "var", "video", "wbr", "gallery", "choose", "option",
};
#define N_HTML_TAGS (sizeof(HTML_TAGS) / sizeof(HTML_TAGS[0]))

typedef struct {
  const char *scheme; // lowercase, without trailing colon
  bool slashes;       // requires "//" after the colon
} Protocol;

static const Protocol PROTOCOLS[] = {
  {"bitcoin", false}, {"ftp", true},    {"ftps", true},   {"geo", false},
  {"git", true},      {"http", true},   {"https", true},  {"irc", true},
  {"ircs", true},     {"magnet", false}, {"mailto", false}, {"mms", true},
  {"news", false},    {"nntp", true},   {"sftp", true},   {"sms", false},
  {"ssh", true},      {"svn", true},    {"tel", false},   {"telnet", true},
  {"urn", false},     {"xmpp", false},
};
#define N_PROTOCOLS (sizeof(PROTOCOLS) / sizeof(PROTOCOLS[0]))

static inline void advance(TSLexer *lexer) { lexer->advance(lexer, false); }

static inline bool is_ascii_alpha(int32_t c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static inline bool is_digit(int32_t c) { return c >= '0' && c <= '9'; }

static inline int32_t to_lower(int32_t c) {
  return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static inline bool is_line_ws(int32_t c) { return c == ' ' || c == '\t'; }

static inline bool at_eol(TSLexer *lexer) {
  return lexer->eof(lexer) || lexer->lookahead == '\n' || lexer->lookahead == '\r';
}

// Characters prose text never contains; each is either the start of another
// construct or a fallback punctuation token (see _punct in the grammar).
static bool is_text_stop(int32_t c) {
  switch (c) {
    case '\n': case '\r': case '\'': case '<': case '>':
    case '[': case ']': case '{': case '}': case '|':
    case '=': case '!': case '&': case '~': case '-':
    case '_': case '#':
      return true;
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// Quotes (bold/italic)
// ---------------------------------------------------------------------------

// Emit the pending zero-width toggle left over from a 4/5+ apostrophe run.
static bool emit_pending_quote(Scanner *s, TSLexer *lexer, const bool *valid) {
  bool pending_b = s->quote_flags & F_PENDING_B;
  bool pending_i = s->quote_flags & F_PENDING_I;
  if (!pending_b && !pending_i) return false;

  lexer->mark_end(lexer); // zero width

  if (pending_b) {
    s->quote_flags &= ~F_PENDING_B;
    if (s->quote_flags & F_BOLD) {
      if (!valid[BOLD_CLOSE]) return false;
      s->quote_flags &= ~(F_BOLD | F_BOLD_INNER);
      lexer->result_symbol = BOLD_CLOSE;
    } else {
      if (!valid[BOLD_OPEN]) return false;
      s->quote_flags |= F_BOLD;
      if (s->quote_flags & F_ITALIC) s->quote_flags |= F_BOLD_INNER;
      lexer->result_symbol = BOLD_OPEN;
    }
    return true;
  }

  s->quote_flags &= ~F_PENDING_I;
  if (s->quote_flags & F_ITALIC) {
    if (!valid[ITALIC_CLOSE]) return false;
    s->quote_flags &= ~F_ITALIC;
    lexer->result_symbol = ITALIC_CLOSE;
  } else {
    if (!valid[ITALIC_OPEN]) return false;
    s->quote_flags |= F_ITALIC;
    lexer->result_symbol = ITALIC_OPEN;
  }
  return true;
}

// Auto-close open bold/italic at end of line (quotes never span lines).
// Zero-width; the innermost span closes first.
static bool close_quotes_at_eol(Scanner *s, TSLexer *lexer, const bool *valid) {
  bool in_i = s->quote_flags & F_ITALIC;
  bool in_b = s->quote_flags & F_BOLD;
  if (!in_i && !in_b) return false;

  bool close_bold_first = in_b && (!in_i || (s->quote_flags & F_BOLD_INNER));

  lexer->mark_end(lexer); // zero width
  if (close_bold_first) {
    if (!valid[BOLD_CLOSE]) return false;
    s->quote_flags &= ~(F_BOLD | F_BOLD_INNER);
    lexer->result_symbol = BOLD_CLOSE;
  } else {
    if (!valid[ITALIC_CLOSE]) return false;
    s->quote_flags &= ~F_ITALIC;
    lexer->result_symbol = ITALIC_CLOSE;
  }
  return true;
}

// Handle a run of 2+ apostrophes. Implements the quote rule from the PEG:
// '' italic toggle, ''' bold toggle, '''' = plain ' + bold, ''''' both, with
// extra apostrophes in longer runs treated as leading plain text (folded into
// the marker token here; only span boundaries matter for highlighting).
static bool scan_quotes(Scanner *s, TSLexer *lexer, const bool *valid) {
  lexer->mark_end(lexer); // enable zero-width emission at run start

  uint32_t n = 0;
  while (lexer->lookahead == '\'') {
    advance(lexer);
    n++;
  }
  if (n < 2) return false;

  bool in_i = s->quote_flags & F_ITALIC;
  bool in_b = s->quote_flags & F_BOLD;
  bool bold_inner = s->quote_flags & F_BOLD_INNER;

  // Closing the outer span with the inner one still open: close the inner
  // span first with a zero-width token; this call does not move the lexer, so
  // the run is re-scanned on the next call with updated state.
  if (n == 2 && in_i && in_b && bold_inner && valid[BOLD_CLOSE]) {
    s->quote_flags &= ~(F_BOLD | F_BOLD_INNER);
    lexer->result_symbol = BOLD_CLOSE;
    return true;
  }
  if (n == 3 && in_b && in_i && !bold_inner && valid[ITALIC_CLOSE]) {
    s->quote_flags &= ~F_ITALIC;
    lexer->result_symbol = ITALIC_CLOSE;
    return true;
  }

  lexer->mark_end(lexer); // token covers the whole run

  if (n == 2) {
    if (in_i && valid[ITALIC_CLOSE]) {
      s->quote_flags &= ~F_ITALIC;
      lexer->result_symbol = ITALIC_CLOSE;
    } else if (valid[ITALIC_OPEN]) {
      // Italic opened second: bold (if open) is the outer span.
      s->quote_flags |= F_ITALIC;
      s->quote_flags &= ~F_BOLD_INNER;
      lexer->result_symbol = ITALIC_OPEN;
    } else {
      return false;
    }
    return true;
  }

  if (n == 3 || n == 4) { // '''' = stray apostrophe + bold marker
    if (in_b && valid[BOLD_CLOSE]) {
      s->quote_flags &= ~(F_BOLD | F_BOLD_INNER);
      lexer->result_symbol = BOLD_CLOSE;
    } else if (valid[BOLD_OPEN]) {
      s->quote_flags |= F_BOLD;
      if (s->quote_flags & F_ITALIC) s->quote_flags |= F_BOLD_INNER;
      lexer->result_symbol = BOLD_OPEN;
    } else {
      return false;
    }
    return true;
  }

  // n >= 5: both toggles; emit one covering the run, queue the other.
  if (in_i && in_b) {
    // Close the inner one now, the outer one next call.
    if (bold_inner) {
      if (!valid[BOLD_CLOSE]) return false;
      s->quote_flags &= ~(F_BOLD | F_BOLD_INNER);
      s->quote_flags |= F_PENDING_I;
      lexer->result_symbol = BOLD_CLOSE;
    } else {
      if (!valid[ITALIC_CLOSE]) return false;
      s->quote_flags &= ~F_ITALIC;
      s->quote_flags |= F_PENDING_B;
      lexer->result_symbol = ITALIC_CLOSE;
    }
    return true;
  }
  if (in_i) {
    if (!valid[ITALIC_CLOSE]) return false;
    s->quote_flags &= ~F_ITALIC;
    s->quote_flags |= F_PENDING_B;
    lexer->result_symbol = ITALIC_CLOSE;
    return true;
  }
  if (in_b) {
    if (!valid[BOLD_CLOSE]) return false;
    s->quote_flags &= ~(F_BOLD | F_BOLD_INNER);
    s->quote_flags |= F_PENDING_I;
    lexer->result_symbol = BOLD_CLOSE;
    return true;
  }
  if (!valid[ITALIC_OPEN]) return false;
  s->quote_flags |= F_ITALIC | F_PENDING_B;
  lexer->result_symbol = ITALIC_OPEN;
  return true;
}

// ---------------------------------------------------------------------------
// Raw-content extension tags
// ---------------------------------------------------------------------------

static bool scan_text(TSLexer *lexer, const bool *valid, uint32_t n,
                      bool boundary);

// Read a tag name (letters then alphanumerics), lowercased. Returns length,
// 0 if too long.
static size_t read_tag_name(TSLexer *lexer, char *buf, size_t cap) {
  size_t len = 0;
  while (is_ascii_alpha(lexer->lookahead) ||
         (len > 0 && is_digit(lexer->lookahead))) {
    if (len >= cap - 1) return 0;
    buf[len++] = (char)to_lower(lexer->lookahead);
    advance(lexer);
  }
  buf[len] = '\0';
  return len;
}

static bool valid_tag_name_end(TSLexer *lexer) {
  int32_t c = lexer->lookahead;
  return c == '>' || c == '/' || is_line_ws(c) || c == '\n' || c == '\r' ||
         lexer->eof(lexer);
}

static int find_tag(const char *const *tags, size_t n, const char *name) {
  for (size_t i = 0; i < n; i++) {
    if (strcmp(name, tags[i]) == 0) return (int)i;
  }
  return -1;
}

// After `<`: a raw extension tag name (remembered for raw_text / the close
// tag) or a known HTML tag name. An unknown name falls through to prose, so
// `<notatag>` renders like MediaWiki does: literally.
static bool scan_tag_name(Scanner *s, TSLexer *lexer, const bool *valid) {
  char buf[20];
  size_t len = read_tag_name(lexer, buf, sizeof(buf));

  if (len > 0 && valid_tag_name_end(lexer)) {
    if (valid[RAW_START_NAME]) {
      int i = find_tag(RAW_TAGS, N_RAW_TAGS, buf);
      if (i >= 0) {
        s->raw_tag = (uint8_t)(i + 1);
        lexer->mark_end(lexer);
        lexer->result_symbol = RAW_START_NAME;
        return true;
      }
    }
    if (valid[HTML_TAG_NAME] &&
        find_tag(HTML_TAGS, N_HTML_TAGS, buf) >= 0) {
      lexer->mark_end(lexer);
      lexer->result_symbol = HTML_TAG_NAME;
      return true;
    }
  }
  // Not a known tag: everything consumed is text-safe.
  lexer->mark_end(lexer);
  return scan_text(lexer, valid, (uint32_t)(len > 0 ? len : 0), false);
}

// '/' of a closing tag: emitted only when a known HTML tag name follows, so
// that `</div>` closes a tag while a stray `</nope>` stays prose.
static bool scan_tag_slash(TSLexer *lexer, const bool *valid) {
  advance(lexer); // '/'
  lexer->mark_end(lexer);
  char buf[20];
  size_t len = read_tag_name(lexer, buf, sizeof(buf));
  if (len > 0 && valid_tag_name_end(lexer) &&
      find_tag(HTML_TAGS, N_HTML_TAGS, buf) >= 0) {
    lexer->result_symbol = TAG_SLASH;
    return true;
  }
  // The consumed '/' and letters are text-safe.
  lexer->mark_end(lexer);
  return scan_text(lexer, valid, (uint32_t)(1 + len), false);
}

// After `</`: the name of the current raw tag.
static bool scan_raw_end_name(Scanner *s, TSLexer *lexer) {
  if (s->raw_tag == 0) return false;
  const char *name = RAW_TAGS[s->raw_tag - 1];
  for (const char *p = name; *p; p++) {
    if (to_lower(lexer->lookahead) != *p) return false;
    advance(lexer);
  }
  if (is_ascii_alpha(lexer->lookahead)) return false;
  lexer->mark_end(lexer);
  s->raw_tag = 0;
  lexer->result_symbol = RAW_END_NAME;
  return true;
}

// Content of a raw tag: everything up to `</name` (or EOF).
static bool scan_raw_text(Scanner *s, TSLexer *lexer) {
  if (s->raw_tag == 0) return false;
  const char *name = RAW_TAGS[s->raw_tag - 1];
  bool consumed = false;

  lexer->mark_end(lexer);
  while (!lexer->eof(lexer)) {
    if (lexer->lookahead == '<') {
      // candidate close tag; extent stays before '<' unless it mismatches
      advance(lexer);
      if (lexer->lookahead == '/') {
        advance(lexer);
        const char *p = name;
        while (*p && to_lower(lexer->lookahead) == *p) {
          advance(lexer);
          p++;
        }
        if (*p == '\0' && !is_ascii_alpha(lexer->lookahead)) {
          break; // real close tag
        }
      }
      consumed = true;
      lexer->mark_end(lexer);
    } else {
      advance(lexer);
      consumed = true;
      lexer->mark_end(lexer);
    }
  }
  if (!consumed) return false;
  lexer->result_symbol = RAW_TEXT;
  return true;
}

static bool scan_raw_self_close(Scanner *s, TSLexer *lexer) {
  if (lexer->lookahead != '/') return false;
  advance(lexer);
  if (lexer->lookahead != '>') return false;
  advance(lexer);
  lexer->mark_end(lexer);
  s->raw_tag = 0;
  lexer->result_symbol = RAW_SELF_CLOSE;
  return true;
}

// ---------------------------------------------------------------------------
// URLs and magic links
// ---------------------------------------------------------------------------

// Try to match a protocol at the current position: letters, ':', optional
// '//'. Returns true (with everything consumed) when a known protocol
// matched. On false, only text-safe characters have been consumed.
static bool match_protocol(TSLexer *lexer) {
  char buf[10];
  size_t len = 0;
  while (is_ascii_alpha(lexer->lookahead)) {
    if (len >= sizeof(buf) - 1) return false;
    buf[len++] = (char)to_lower(lexer->lookahead);
    advance(lexer);
  }
  if (len == 0 || lexer->lookahead != ':') return false;
  buf[len] = '\0';

  const Protocol *proto = NULL;
  for (size_t i = 0; i < N_PROTOCOLS; i++) {
    if (strcmp(buf, PROTOCOLS[i].scheme) == 0) {
      proto = &PROTOCOLS[i];
      break;
    }
  }
  if (!proto) return false;
  advance(lexer); // ':'
  if (proto->slashes) {
    if (lexer->lookahead != '/') return false;
    advance(lexer);
    if (lexer->lookahead != '/') return false;
    advance(lexer);
  }
  return true;
}

static bool is_url_stop(int32_t c, bool bracket) {
  if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '<' ||
      c == '>' || c == '"' || c == '\'') {
    return true;
  }
  if (c == '[' || c == ']') return true;
  if (!bracket && (c == '{' || c == '}' || c == '|')) return true;
  return false;
}

// Trailing punctuation that a bare URL does not end with (PEG autourl).
static bool is_url_trim(int32_t c) {
  switch (c) {
    case '.': case ',': case ';': case ':': case '!': case '?': case ')':
      return true;
    default:
      return false;
  }
}

// The protocol has already been consumed. Scan the URL body; for bare URLs
// trailing punctuation is left out of the token via delayed mark_end.
static bool scan_url_body(TSLexer *lexer, bool bracket, enum TokenType sym) {
  bool has_body = false;
  while (!lexer->eof(lexer) && !is_url_stop(lexer->lookahead, bracket)) {
    int32_t c = lexer->lookahead;
    advance(lexer);
    if (bracket || !is_url_trim(c)) {
      lexer->mark_end(lexer);
      has_body = true;
    }
  }
  if (!has_body) return false;
  lexer->result_symbol = sym;
  return true;
}

// Full magic link starting at ISBN/RFC/PMID. Everything consumed on failure
// is text-safe (letters, spaces, digits).
static bool scan_magic_link(TSLexer *lexer) {
  static const char *const kws[] = {"ISBN", "RFC", "PMID"};
  int kw = -1;
  for (int i = 0; i < 3; i++) {
    if (lexer->lookahead == kws[i][0]) { kw = i; break; }
  }
  if (kw < 0) return false;
  for (const char *p = kws[kw]; *p; p++) {
    if (lexer->lookahead != *p) return false;
    advance(lexer);
  }
  if (!is_line_ws(lexer->lookahead)) return false;
  while (is_line_ws(lexer->lookahead)) advance(lexer);
  if (!is_digit(lexer->lookahead)) return false;

  if (kw == 0) {
    // ISBN: digits with dashes/spaces, optionally ending in X.
    while (true) {
      int32_t c = lexer->lookahead;
      if (is_digit(c) || c == 'X' || c == 'x') {
        advance(lexer);
        lexer->mark_end(lexer);
      } else if (c == '-' || c == ' ') {
        advance(lexer); // only kept if followed by more digits
      } else {
        break;
      }
    }
  } else {
    while (is_digit(lexer->lookahead)) {
      advance(lexer);
      lexer->mark_end(lexer);
    }
  }
  lexer->result_symbol = MAGIC_LINK;
  return true;
}

// Cheap prefix check used to stop a text token before a magic link:
// KEYWORD, whitespace, digit. Consumes only text-safe characters.
static bool magic_link_prefix(TSLexer *lexer) {
  static const char *const kws[] = {"ISBN", "RFC", "PMID"};
  int kw = -1;
  for (int i = 0; i < 3; i++) {
    if (lexer->lookahead == kws[i][0]) { kw = i; break; }
  }
  if (kw < 0) return false;
  for (const char *p = kws[kw]; *p; p++) {
    if (lexer->lookahead != *p) return false;
    advance(lexer);
  }
  if (!is_line_ws(lexer->lookahead)) return false;
  while (is_line_ws(lexer->lookahead)) advance(lexer);
  return is_digit(lexer->lookahead);
}

// First letters of the protocols above, for a cheap trigger test.
static bool is_protocol_start(int32_t c) {
  switch (to_lower(c)) {
    case 'b': case 'f': case 'g': case 'h': case 'i': case 'm': case 'n':
    case 's': case 't': case 'u': case 'x':
      return true;
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

// Scan prose. `n` characters may already have been consumed (and marked) by a
// caller that fell through here. Mirrors Parsoid's urltext: stops at special
// characters and, at word boundaries, before magic links and bare URLs.
static bool scan_text(TSLexer *lexer, const bool *valid, uint32_t n,
                      bool boundary) {
  if (!valid[TEXT]) return false;

  while (!lexer->eof(lexer)) {
    int32_t c = lexer->lookahead;
    if (is_text_stop(c)) break;

    if (boundary && is_ascii_alpha(c)) {
      bool is_magic_candidate =
          valid[MAGIC_LINK] && (c == 'I' || c == 'R' || c == 'P');
      bool is_url_candidate = valid[AUTO_URL] && is_protocol_start(c);
      if (is_magic_candidate || is_url_candidate) {
        if (n == 0) {
          // Token start: try to emit the construct itself.
          if (is_magic_candidate && scan_magic_link(lexer)) return true;
          // scan_magic_link consumed text-safe chars on failure; a URL can
          // no longer start at this position unless nothing was consumed.
          if (is_url_candidate && !is_magic_candidate && match_protocol(lexer) &&
              scan_url_body(lexer, false, AUTO_URL)) {
            return true;
          }
          // Fall through: absorb whatever was consumed as text.
          lexer->mark_end(lexer);
          n++; // at least one char was consumed on any failure path above
          boundary = false;
          continue;
        }
        // Mid-token: if a construct starts here, end the text before it.
        bool starts = is_magic_candidate ? magic_link_prefix(lexer)
                                         : match_protocol(lexer);
        if (starts) break;
        // Not a construct: absorb the characters looked at.
        lexer->mark_end(lexer);
        boundary = false;
        continue;
      }
    }

    boundary = !(is_ascii_alpha(c) || is_digit(c) || c >= 128);
    advance(lexer);
    lexer->mark_end(lexer);
    n++;
  }

  if (n == 0) return false;
  lexer->result_symbol = TEXT;
  return true;
}

// ---------------------------------------------------------------------------
// Start-of-line constructs
// ---------------------------------------------------------------------------

static bool scan_heading_open(TSLexer *lexer) {
  uint32_t n = 0;
  while (lexer->lookahead == '=') {
    advance(lexer);
    n++;
    if (n <= 6) lexer->mark_end(lexer);
  }
  if (n == 0) return false;

  // Valid only if the rest of the line has content and ends (modulo trailing
  // blanks) with '='. Lookahead only; the token extent is already marked.
  bool has_content = false;
  int32_t last_non_ws = 0;
  while (!at_eol(lexer)) {
    int32_t c = lexer->lookahead;
    if (!is_line_ws(c)) {
      has_content = true;
      last_non_ws = c;
    }
    advance(lexer);
  }
  if (!has_content || last_non_ws != '=') return false;
  lexer->result_symbol = HEADING_OPEN;
  return true;
}

static bool scan_heading_close(TSLexer *lexer) {
  uint32_t n = 0;
  while (lexer->lookahead == '=') {
    advance(lexer);
    n++;
  }
  if (n == 0) return false;
  lexer->mark_end(lexer);
  while (is_line_ws(lexer->lookahead)) advance(lexer);
  if (!at_eol(lexer)) return false;
  lexer->result_symbol = HEADING_CLOSE;
  return true;
}

static bool scan_list_marker_or_redirect(TSLexer *lexer, const bool *valid) {
  int32_t first = lexer->lookahead;
  uint32_t n = 0;
  while (lexer->lookahead == '*' || lexer->lookahead == '#' ||
         lexer->lookahead == ':' || lexer->lookahead == ';') {
    advance(lexer);
    n++;
  }
  if (n == 0) return false;
  lexer->mark_end(lexer);

  if (valid[REDIRECT_KEYWORD] && n == 1 && first == '#') {
    static const char kw[] = "redirect";
    const char *p = kw;
    while (*p && to_lower(lexer->lookahead) == *p) {
      advance(lexer);
      p++;
    }
    if (*p == '\0' && !is_ascii_alpha(lexer->lookahead)) {
      lexer->mark_end(lexer);
      lexer->result_symbol = REDIRECT_KEYWORD;
      return true;
    }
  }

  if (!valid[LIST_MARKER]) return false;
  lexer->result_symbol = LIST_MARKER;
  return true;
}

static bool scan_table_pipe(TSLexer *lexer, const bool *valid) {
  advance(lexer); // '|'
  lexer->mark_end(lexer);
  int32_t c = lexer->lookahead;
  if (c == '}' && valid[TABLE_END]) {
    advance(lexer);
    lexer->mark_end(lexer);
    lexer->result_symbol = TABLE_END;
    return true;
  }
  if (c == '-' && valid[TABLE_ROW]) {
    while (lexer->lookahead == '-') advance(lexer);
    lexer->mark_end(lexer);
    lexer->result_symbol = TABLE_ROW;
    return true;
  }
  if (c == '+' && valid[TABLE_CAPTION]) {
    advance(lexer);
    lexer->mark_end(lexer);
    lexer->result_symbol = TABLE_CAPTION;
    return true;
  }
  if (valid[TABLE_CELL_SOL]) {
    lexer->result_symbol = TABLE_CELL_SOL;
    return true;
  }
  return false;
}

static bool scan_hr(TSLexer *lexer) {
  uint32_t n = 0;
  while (lexer->lookahead == '-') {
    advance(lexer);
    n++;
  }
  if (n < 4) return false;
  lexer->mark_end(lexer);
  lexer->result_symbol = HR;
  return true;
}

// A single leading space starts an indent-pre block if the line is not blank.
// On a blank line the consumed spaces are re-emitted as text.
static bool scan_indent_pre(TSLexer *lexer, const bool *valid) {
  advance(lexer); // the space
  lexer->mark_end(lexer);
  uint32_t extra = 0;
  while (is_line_ws(lexer->lookahead)) {
    advance(lexer);
    extra++;
  }
  if (!at_eol(lexer)) {
    lexer->result_symbol = INDENT_PRE;
    return true;
  }
  // Blank line: consume it as text instead (internal tokens cannot lex
  // spaces, so falling back to the internal lexer would be an error).
  lexer->mark_end(lexer);
  return scan_text(lexer, valid, 1 + extra, true);
}

// ---------------------------------------------------------------------------
// Main scan
// ---------------------------------------------------------------------------

static bool scan(Scanner *s, TSLexer *lexer, const bool *valid) {
  if (valid[ERROR_SENTINEL]) return false; // error recovery: internal lexer only

  if (s->quote_flags & (F_PENDING_I | F_PENDING_B)) {
    if (emit_pending_quote(s, lexer, valid)) return true;
    s->quote_flags &= ~(F_PENDING_I | F_PENDING_B);
  }

  if (at_eol(lexer)) {
    if (close_quotes_at_eol(s, lexer, valid)) return true;
    if (lexer->eof(lexer)) return false;
  }

  int32_t c = lexer->lookahead;

  if (lexer->get_column(lexer) == 0 && !lexer->eof(lexer)) {
    switch (c) {
      case '{':
        if (valid[TABLE_START]) {
          advance(lexer);
          if (lexer->lookahead == '|') {
            advance(lexer);
            lexer->mark_end(lexer);
            lexer->result_symbol = TABLE_START;
            return true;
          }
          return false;
        }
        break;
      case '|':
        if (valid[TABLE_END] || valid[TABLE_ROW] || valid[TABLE_CAPTION] ||
            valid[TABLE_CELL_SOL]) {
          return scan_table_pipe(lexer, valid);
        }
        break;
      case '!':
        if (valid[TABLE_HEADER_SOL]) {
          advance(lexer);
          lexer->mark_end(lexer);
          lexer->result_symbol = TABLE_HEADER_SOL;
          return true;
        }
        break;
      case '*': case '#': case ':': case ';':
        if (valid[LIST_MARKER] || valid[REDIRECT_KEYWORD]) {
          return scan_list_marker_or_redirect(lexer, valid);
        }
        break;
      case '=':
        if (valid[HEADING_OPEN]) return scan_heading_open(lexer);
        break;
      case '-':
        if (valid[HR] && scan_hr(lexer)) return true;
        // A '-' run shorter than 4 was discarded; the internal lexer
        // re-lexes it (as /-+/ or '-{').
        if (valid[HR]) return false;
        break;
      case ' ':
        if (valid[INDENT_PRE]) return scan_indent_pre(lexer, valid);
        break;
      default:
        break;
    }
  }

  if (valid[HEADING_CLOSE] && c == '=') {
    return scan_heading_close(lexer);
  }

  if (c == '\'' &&
      (valid[ITALIC_OPEN] || valid[ITALIC_CLOSE] || valid[BOLD_OPEN] ||
       valid[BOLD_CLOSE])) {
    return scan_quotes(s, lexer, valid);
  }

  if ((valid[RAW_START_NAME] || valid[HTML_TAG_NAME]) && is_ascii_alpha(c)) {
    return scan_tag_name(s, lexer, valid);
  }
  if (valid[RAW_END_NAME]) return scan_raw_end_name(s, lexer);
  if (valid[RAW_TEXT]) return scan_raw_text(s, lexer);
  if (valid[RAW_SELF_CLOSE] && c == '/') return scan_raw_self_close(s, lexer);
  if (valid[TAG_SLASH] && c == '/') return scan_tag_slash(lexer, valid);

  if (valid[BRACKET_URL] && is_ascii_alpha(c)) {
    if (match_protocol(lexer) && scan_url_body(lexer, true, BRACKET_URL)) {
      return true;
    }
    // Everything consumed above is text-safe (letters, ':', '/'); absorb it
    // as text so a GLR branch that expects prose here can proceed.
    lexer->mark_end(lexer);
    return scan_text(lexer, valid, 1, false);
  }

  return scan_text(lexer, valid, 0, true);
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

void *tree_sitter_wikitext_external_scanner_create(void) {
  Scanner *s = calloc(1, sizeof(Scanner));
  return s;
}

void tree_sitter_wikitext_external_scanner_destroy(void *payload) {
  free(payload);
}

unsigned tree_sitter_wikitext_external_scanner_serialize(void *payload,
                                                         char *buffer) {
  Scanner *s = (Scanner *)payload;
  buffer[0] = (char)s->quote_flags;
  buffer[1] = (char)s->raw_tag;
  return 2;
}

void tree_sitter_wikitext_external_scanner_deserialize(void *payload,
                                                       const char *buffer,
                                                       unsigned length) {
  Scanner *s = (Scanner *)payload;
  if (length >= 2) {
    s->quote_flags = (uint8_t)buffer[0];
    s->raw_tag = (uint8_t)buffer[1];
  } else {
    s->quote_flags = 0;
    s->raw_tag = 0;
  }
}

bool tree_sitter_wikitext_external_scanner_scan(void *payload, TSLexer *lexer,
                                                const bool *valid_symbols) {
  return scan((Scanner *)payload, lexer, valid_symbols);
}
