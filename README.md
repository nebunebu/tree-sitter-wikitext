# tree-sitter-wikitext

> [!CAUTION]
> This entire repo is code generated.

A [tree-sitter](https://tree-sitter.github.io/) grammar for MediaWiki
wikitext (`.wiki`, `.mediawiki`, `.mw`), modeled on Parsoid's PEG tokenizer
(`Grammar.pegphp`, kept in this repo as the syntax reference).

## Design notes

- **Combined wiki + HTML tokenizer**, like Parsoid. HTML tags are parsed
  natively as flat (unpaired) `html_tag` nodes — wiki HTML is routinely
  unbalanced, and wikitext keeps parsing inside HTML blocks. Only known
  HTML/extension tag names are recognized (Parsoid's `isXMLTag`); `<nope>`
  is prose, exactly as MediaWiki renders it.
- **Raw-content extension tags** (`nowiki`, `pre`, `syntaxhighlight`,
  `source`, `math`, `chem`, `score`, `templatedata`, `graph`, `mapframe`,
  `maplink`, `timeline`, `hiero`, `imagemap`, `inputbox`, `charinsert`,
  `categorytree`) are `ext_tag` nodes whose content is a single `raw_text`
  token lexed by the external scanner — the same technique tree-sitter-html
  uses for `<script>`. This makes language injection reliable:
  `templatedata`/`graph` → JSON, `syntaxhighlight lang=…` → that language,
  `math` → LaTeX, `score` → LilyPond (see `queries/injections.scm`).
- **Transparent extension tags** (`ref`, `poem`, `gallery`, `includeonly`,
  `noinclude`, …) parse like HTML tags and their content stays wikitext, so
  `<ref>{{cite web|…}}</ref>` highlights fully.
- The **external scanner** also handles the start-of-line constructs
  (headings, list markers, tables, `----`, indent-pre, `#REDIRECT`),
  bold/italic apostrophe runs with MediaWiki's per-line auto-close, and
  prose text with Parsoid-style lookahead stops before bare URLs and
  ISBN/RFC/PMID magic links.
- Language-converter blocks (`-{ … }-`) and magic links are supported.

## Building and testing

Everything runs from the dev shell (NixOS-friendly; nothing global needed):

```sh
nix develop            # or: nix-shell
tree-sitter generate   # grammar.js -> src/parser.c
tree-sitter test       # corpus tests in test/corpus/
tree-sitter parse examples/Article.wiki
```

## Neovim

```sh
./scripts/install-nvim.sh
```

This builds `wikitext.so` and installs, under `~/.config/nvim`:

- `parser/wikitext.so`
- `queries/wikitext/{highlights,injections,folds}.scm`
- `plugin/wikitext-treesitter.lua` — registers filetypes `wiki`,
  `mediawiki`, `wikitext` for the `wikitext` language (overriding any
  earlier registration, e.g. one pointing at another mediawiki parser).

Highlighting is Neovim's builtin `vim.treesitter.start()`; injections need
the target parsers (`json`, `latex`, …) to be installed, e.g. via
nvim-treesitter.

### Nix flake

`packages.default` builds the grammar with `pkgs.tree-sitter.buildGrammar`
(output layout: `parser` plus `queries/`), suitable for wiring into a
nixified Neovim the same way as other out-of-tree grammars:

```nix
tree-sitter-wikitext = inputs.tree-sitter-wikitext.packages.${system}.default;
# then symlink ${tree-sitter-wikitext}/parser as parser/wikitext.so
# and register: vim.treesitter.language.register("wikitext", { "wiki", ... })
```

## Node types (highlight-relevant)

`heading`/`heading_marker`, `list_item`/`list_marker`, `preformatted`,
`horizontal_rule`, `redirect`, `table` (`table_row`, `table_caption`,
`table_cell`, `table_header_cell`, `table_cell_attributes`), `template`
(`template_name`, `template_argument`, `argument_name`), `parser_function`,
`parameter`, `wikilink`/`link_target`, `external_link`/`url`/`link_label`,
`magic_link`, `bold`, `italic`, `html_tag`/`ext_tag` (`tag_name`,
`attribute`, `attribute_name`, `attribute_value`, `raw_text`), `entity`,
`behavior_switch`, `signature`, `language_converter`, `comment`, `text`.
