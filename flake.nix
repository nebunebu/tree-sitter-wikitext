{
  description = "Tree-sitter grammar for MediaWiki wikitext";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
        "x86_64-darwin"
        "aarch64-darwin"
      ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      packages = forAllSystems (pkgs: rec {
        tree-sitter-wikitext = pkgs.tree-sitter.buildGrammar {
          language = "wikitext";
          version = "0.1.0";
          src = self;
        };
        default = tree-sitter-wikitext;
      });

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          packages = with pkgs; [
            tree-sitter
            nodejs
            gcc
          ];
        };
      });
    };
}
