#!/usr/bin/env bash
# Build the wikitext parser and install it as a drop-in into Neovim's site
# directory (stdpath('data')/site, which respects NVIM_APPNAME and wrapped
# configs): parser/wikitext.so, queries/wikitext/*.scm, and
# plugin/wikitext-treesitter.lua.
#
# The plugin file registers the wiki filetypes for this parser; plugin/
# scripts run after the main config, so it overrides an existing
# "mediawiki" registration.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
nvim_dir="$(nvim --headless +'lua io.write(vim.fn.stdpath("data") .. "/site")' +q 2>/dev/null)"
if [ -z "$nvim_dir" ]; then
  nvim_dir="${XDG_CONFIG_HOME:-$HOME/.config}/nvim"
fi

echo "Building parser..."
if command -v tree-sitter >/dev/null 2>&1; then
  (cd "$repo" && tree-sitter build -o "$repo/wikitext.so")
else
  nix-shell -p tree-sitter gcc nodejs --run \
    "cd '$repo' && tree-sitter build -o '$repo/wikitext.so'"
fi

echo "Installing into $nvim_dir ..."
mkdir -p "$nvim_dir/parser" "$nvim_dir/queries/wikitext" "$nvim_dir/plugin"
cp "$repo/wikitext.so" "$nvim_dir/parser/wikitext.so"
cp "$repo"/queries/*.scm "$nvim_dir/queries/wikitext/"

cat > "$nvim_dir/plugin/wikitext-treesitter.lua" <<'EOF'
-- Installed by tree-sitter-wikitext (scripts/install-nvim.sh).
-- Sourced after the main config, so this wins over an earlier
-- vim.treesitter.language.register("mediawiki", ...) call.
vim.filetype.add({
  extension = { wiki = "mediawiki", mediawiki = "mediawiki", mw = "mediawiki" },
})
vim.treesitter.language.register("wikitext", { "wiki", "mediawiki", "wikitext" })
EOF

echo "Done. Open a .wiki file; :InspectTree should show wikitext nodes."
