; Comments
(comment) @comment

; Headings: whole line styled, level derived from the opening marker
(heading) @markup.heading
((heading . (heading_marker) @_o) @markup.heading.1 (#eq? @_o "="))
((heading . (heading_marker) @_o) @markup.heading.2 (#eq? @_o "=="))
((heading . (heading_marker) @_o) @markup.heading.3 (#eq? @_o "==="))
((heading . (heading_marker) @_o) @markup.heading.4 (#eq? @_o "===="))
((heading . (heading_marker) @_o) @markup.heading.5 (#eq? @_o "====="))
((heading . (heading_marker) @_o) @markup.heading.6 (#eq? @_o "======"))

; Formatting
(bold) @markup.strong
(italic) @markup.italic

; Lists / rules
(list_marker) @markup.list
(horizontal_rule) @punctuation.special

; Links
(wikilink ["[[" "]]" "|"] @punctuation.bracket)
(wikilink target: (link_target) @markup.link.url)
(external_link ["[" "]"] @punctuation.bracket)
(url) @markup.link.url
(magic_link) @markup.link.url
(link_label) @markup.link.label

; Templates and parameters
(template ["{{" "}}"] @punctuation.bracket)
(template name: (template_name) @function.call)
(template_argument "|" @punctuation.delimiter)
(template_argument name: (argument_name) @variable.parameter)
(template_argument name: (argument_name) . "=" @operator)
(parser_function ["{{" "}}"] @punctuation.bracket)
(parser_function ":" @punctuation.delimiter)
(parser_function name: (parser_function_name) @function.builtin)
(parameter ["{{{" "}}}"] @punctuation.special)
(parameter "|" @punctuation.delimiter)
(parameter name: (parameter_name) @variable.parameter)

; HTML and extension tags
(html_tag ["<" ">" "/"] @tag.delimiter)
(ext_tag ["<" ">" "</"] @tag.delimiter)
(tag_name) @tag
(attribute_name) @tag.attribute
(attribute_value) @string
(attribute "=" @operator)

; Tables
(table ["{|" "|}"] @punctuation.special)
(table_row "|-" @punctuation.special)
(table_caption "|+" @punctuation.special)
(table_cell ["|" "||"] @punctuation.special)
(table_header_cell ["!" "!!"] @punctuation.special)
(table_cell_attributes) @attribute
(table_header_cell (table_cell_attributes) "|" @punctuation.special)
(table_cell (table_cell_attributes) "|" @punctuation.special)
(table_caption (table_cell_attributes) "|" @punctuation.special)

; Misc
(entity) @string.escape
(behavior_switch) @keyword.directive
(signature) @keyword
(redirect_keyword) @keyword.import
(language_converter ["-{" "}-"] @punctuation.special)
