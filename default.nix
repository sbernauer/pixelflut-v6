{
  nixpkgs ? import <nixpkgs> {},
  nixpkgsUnstable ? import (fetchTarball "https://github.com/NixOS/nixpkgs/archive/nixos-unstable.tar.gz") {},
}:

nixpkgs.mkShell {
  buildInputs = [
    nixpkgs.pkg-config
    nixpkgs.dpdk
    # nixpkgsUnstable.dpdk # Uncomment to use dpdk from nixpkgs-unstable
    nixpkgs.imagemagick
    nixpkgs.python312Packages.pyelftools # needed for dpdk-pmdinfo.py

    # For debugging
    nixpkgs.gdb
  ];
}
