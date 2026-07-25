; JSON-bearing extension tags
((ext_tag
   name: (tag_name) @_name
   content: (raw_text) @injection.content)
 (#any-of? @_name "templatedata" "graph" "mapframe" "maplink")
 (#set! injection.language "json"))

; Math is TeX
((ext_tag
   name: (tag_name) @_name
   content: (raw_text) @injection.content)
 (#any-of? @_name "math" "chem" "ce")
 (#set! injection.language "latex"))

; Musical scores are LilyPond
((ext_tag
   name: (tag_name) @_name
   content: (raw_text) @injection.content)
 (#eq? @_name "score")
 (#set! injection.language "lilypond"))

; Syntax highlighting blocks take their language from the lang attribute
((ext_tag
   name: (tag_name) @_name
   (attribute
     (attribute_name) @_attr
     (attribute_value) @injection.language)
   content: (raw_text) @injection.content)
 (#any-of? @_name "syntaxhighlight" "source")
 (#eq? @_attr "lang"))
