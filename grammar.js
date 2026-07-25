/**
 * Tree-sitter grammar for MediaWiki wikitext.
 *
 * Modeled on Parsoid's PEG tokenizer (Grammar.pegphp, kept in this repo as a
 * reference): a combined wiki + HTML tokenizer. HTML tags are parsed natively
 * (flat, unpaired), extension tags with raw content (nowiki, syntaxhighlight,
 * templatedata, ...) have their content lexed by the external scanner so that
 * language injection works on them.
 *
 * Start-of-line constructs (headings, lists, tables, hr, indent-pre) are
 * external tokens gated on column 0, mirroring Parsoid's sol-context.
 */

module.exports = grammar({
  name: 'wikitext',

  externals: $ => [
    $.list_marker,        // run of [*#:;] at column 0
    $._heading_open,      // ={1,6} at column 0, on a line validated as heading
    $._heading_close,     // =+ [ \t]* before end of line
    $._hr,                // ----+ at column 0
    $._indent_pre,        // single leading space at column 0, non-blank line
    $._table_start,       // {| at column 0
    $._table_end,         // |} at column 0
    $._table_row,         // |-+ at column 0
    $._table_caption,     // |+ at column 0
    $._table_cell_sol,    // | at column 0
    $._table_header_sol,  // ! at column 0
    $._redirect_keyword,  // #REDIRECT
    $.text,               // prose; stops before magic links / bare URLs
    $._bracket_url,       // URL inside [ ] (no trailing-punctuation trim)
    $._auto_url,          // free-standing URL (trailing punctuation trimmed)
    $.magic_link,         // ISBN ... / RFC nnn / PMID nnn
    $._italic_open,
    $._italic_close,      // may be zero-width at end of line
    $._bold_open,
    $._bold_close,        // may be zero-width at end of line
    $._raw_start_name,    // name of a raw-content extension tag, after <
    $._raw_end_name,      // matching name in the closing tag
    $.raw_text,           // raw content of an extension tag
    $._raw_self_close,    // /> closing a raw-content extension tag
    $._error_sentinel,
  ],

  extras: $ => [$.comment],

  conflicts: $ => [
    [$._inline_core, $.argument_name],
    [$._inline_core, $.table_cell_attributes],
    [$._punct, $.argument_name],
    [$._punct, $.table_cell_attributes],
    [$.argument_name, $._arg_content],
  ],

  rules: {
    document: $ => repeat($._block_element),

    _block_element: $ => choice(
      $.redirect,
      $.heading,
      $.list_item,
      $.preformatted,
      $.table,
      $.horizontal_rule,
      $._inline_core,
      $._punct,
      '|',
      $._newline,
    ),

    _newline: _ => token(/\r?\n/),
    _sp: _ => token(/[ \t]+/),
    _tag_ws: _ => token(/[ \t\r\n]+/),

    // ------------------------------------------------------------------
    // Inline element sets
    // ------------------------------------------------------------------

    _inline_core: $ => choice(
      $.text,
      $.template,
      $.parameter,
      $.parser_function,
      $.wikilink,
      $.external_link,
      alias($._auto_url, $.url),
      $.magic_link,
      $.bold,
      $.italic,
      $.html_tag,
      $.ext_tag,
      $.entity,
      $.behavior_switch,
      $.signature,
      $.language_converter,
    ),

    // Fallback punctuation: special characters appearing in plain-text
    // position. Anonymous tokens so they stay invisible to most queries.
    _punct: $ => choice(
      "'", '<', '>', '[', ']', '{', '}', '=', '!', '&',
      token(/~+/), token(/-+/), token(/_+/), token(prec(1, /#+/)),
    ),

    _inline: $ => choice($._inline_core, $._punct),

    // ------------------------------------------------------------------
    // Blocks
    // ------------------------------------------------------------------

    redirect: $ => prec.right(seq(
      alias($._redirect_keyword, $.redirect_keyword),
      optional($.text),
      optional($.wikilink),
    )),

    heading: $ => seq(
      alias($._heading_open, $.heading_marker),
      repeat($._inline),
      alias($._heading_close, $.heading_marker),
    ),

    list_item: $ => prec.right(seq($.list_marker, repeat($._inline))),

    preformatted: $ => prec.right(seq($._indent_pre, repeat($._inline))),

    horizontal_rule: $ => $._hr,

    // ------------------------------------------------------------------
    // Tables
    // ------------------------------------------------------------------

    table: $ => seq(
      alias($._table_start, '{|'),
      optional($.table_attributes),
      $._newline,
      repeat($._table_content),
      alias($._table_end, '|}'),
    ),

    _table_content: $ => choice(
      $.table_row,
      $.table_caption,
      $.table_cell,
      $.table_header_cell,
      $.table,
      $.list_item,
      $.heading,
      $.horizontal_rule,
      $._inline_core,
      $._punct,
      '|',
      $._newline,
    ),

    table_row: $ => seq(
      alias($._table_row, '|-'),
      optional($.table_attributes),
    ),

    table_caption: $ => prec.right(seq(
      alias($._table_caption, '|+'),
      optional(prec.dynamic(2, seq($.table_cell_attributes, '|'))),
      repeat($._inline),
    )),

    table_cell: $ => prec.right(seq(
      choice(alias($._table_cell_sol, '|'), '||'),
      optional(prec.dynamic(2, seq($.table_cell_attributes, '|'))),
      repeat($._inline),
    )),

    table_header_cell: $ => prec.right(seq(
      choice(alias($._table_header_sol, '!'), '!!'),
      optional(prec.dynamic(2, seq($.table_cell_attributes, '|'))),
      repeat($._inline),
    )),

    // Attribute segment of a table cell (`| attrs | content`). Parsed with
    // the same tokens as inline content; GLR + dynamic precedence picks this
    // interpretation when a single `|` delimiter follows on the same line.
    table_cell_attributes: $ => repeat1(choice(
      $.text,
      $.entity,
      $.template,
      $.parameter,
      '=', "'",
      token(/~+/), token(/-+/), token(/_+/), token(prec(1, /#+/)),
    )),

    // Attributes on {| and |- lines: rest of the line, parsed structurally.
    table_attributes: $ => repeat1(choice($._sp, $.attribute)),

    // ------------------------------------------------------------------
    // Templates, parameters, parser functions
    // ------------------------------------------------------------------

    template: $ => seq(
      '{{',
      field('name', optional($.template_name)),
      repeat($.template_argument),
      '}}',
    ),

    // No /#+/ here: a name starting with '#' lexes as parser_function_name.
    template_name: $ => repeat1(choice(
      $.text, $.entity, $.template, $.parameter,
      "'", token(/-+/), token(/_+/), $._newline,
    )),

    template_argument: $ => seq(
      '|',
      optional(choice(
        prec.dynamic(1, seq(field('name', $.argument_name), '=', repeat($._arg_content))),
        repeat1($._arg_content),
      )),
    ),

    argument_name: $ => repeat1(choice(
      $.text, $.entity, $.template, $.parameter,
      "'", token(/-+/), token(/_+/), token(prec(1, /#+/)), $._newline,
    )),

    _arg_content: $ => choice(
      $._inline,
      $._newline,
      $.list_item,
    ),

    parser_function: $ => seq(
      '{{',
      field('name', $.parser_function_name),
      optional(seq(':', repeat($._arg_content))),
      repeat($.template_argument),
      '}}',
    ),

    parser_function_name: _ => token(/#[a-zA-Z][a-zA-Z0-9_]*/),

    parameter: $ => seq(
      '{{{',
      field('name', optional($.parameter_name)),
      optional(seq('|', repeat($._arg_content))),
      '}}}',
    ),

    parameter_name: $ => repeat1(choice(
      $.text, $.entity, $.template, $.parameter,
      "'", token(/-+/), token(/_+/), token(prec(1, /#+/)), $._newline,
    )),

    // ------------------------------------------------------------------
    // Links
    // ------------------------------------------------------------------

    wikilink: $ => seq(
      '[[',
      field('target', optional($.link_target)),
      repeat($._wikilink_segment),
      ']]',
    ),

    _wikilink_segment: $ => seq('|', repeat(choice($._inline, $._newline))),

    link_target: $ => repeat1(choice(
      $.text, $.entity, $.template, $.parameter,
      "'", '&', token(/-+/), token(/_+/), token(prec(1, /#+/)),
    )),

    external_link: $ => prec.dynamic(2, seq(
      '[',
      alias($._bracket_url, $.url),
      optional(seq($._sp, optional($.link_label))),
      ']',
    )),

    // Label content: like _inline but without a bare ']', which closes the
    // link.
    link_label: $ => repeat1(choice(
      $._inline_core,
      "'", '<', '>', '[', '{', '}', '=', '!', '&',
      token(/~+/), token(/-+/), token(/_+/), token(prec(1, /#+/)),
    )),

    // ------------------------------------------------------------------
    // Bold / italic
    // ------------------------------------------------------------------

    bold: $ => seq($._bold_open, repeat($._inline), $._bold_close),

    italic: $ => seq($._italic_open, repeat($._inline), $._italic_close),

    // ------------------------------------------------------------------
    // HTML tags and extension tags
    // ------------------------------------------------------------------

    html_tag: $ => prec.dynamic(3, seq(
      '<',
      optional('/'),
      $.tag_name,
      repeat(seq($._tag_ws, optional($.attribute))),
      optional('/'),
      '>',
    )),

    tag_name: _ => token(/[a-zA-Z][a-zA-Z0-9-]*/),

    // Raw-content extension tag; content is lexed by the external scanner so
    // that other languages can be injected (templatedata -> json, ...).
    ext_tag: $ => seq(
      '<',
      field('name', alias($._raw_start_name, $.tag_name)),
      repeat(seq($._tag_ws, optional($.attribute))),
      choice(
        $._raw_self_close,
        seq(
          '>',
          optional(field('content', $.raw_text)),
          token('</'),
          alias($._raw_end_name, $.tag_name),
          optional($._tag_ws),
          '>',
        ),
      ),
    ),

    attribute: $ => prec.right(seq(
      $.attribute_name,
      optional(seq(
        '=',
        choice(
          seq('"', optional(alias(token(/[^"\n]+/), $.attribute_value)), '"'),
          seq("'", optional(alias(token(/[^'\n]+/), $.attribute_value)), "'"),
          alias(token(/[^ \t\r\n<>'"|]+/), $.attribute_value),
        ),
      )),
    )),

    attribute_name: _ => token(/[^ \t\r\n<>=\/'"|]+/),

    // ------------------------------------------------------------------
    // Misc inline
    // ------------------------------------------------------------------

    comment: _ => token(seq(
      '<!--',
      repeat(choice(/[^-]/, seq('-', /[^-]/))),
      '--',
      repeat('-'),
      '>',
    )),

    entity: _ => token(choice(
      /&[a-zA-Z][a-zA-Z0-9]{1,31};/,
      /&#[0-9]{1,7};/,
      /&#[xX][0-9a-fA-F]{1,6};/,
    )),

    behavior_switch: _ => token(/__[a-zA-Z][a-zA-Z0-9]*__/),

    signature: _ => token(prec(1, /~{3,5}/)),

    language_converter: $ => seq(
      '-{',
      repeat(choice($._inline, '|', $._newline)),
      '}-',
    ),
  },
});
