{
  pkgs ? import <nixpkgs> { },
}:
pkgs.mkShell {
  packages = with pkgs; [
    tree-sitter
    nodejs
    gcc
  ];
}
