{ nixpkgs ? import <nixpkgs> {} }:

nixpkgs.mkShell {
  buildInputs = [
    nixpkgs.pkg-config
    nixpkgs.dpdk
    nixpkgs.imagemagick
    nixpkgs.python312Packages.pyelftools # needed for dpdk-pmdinfo.py

    # For debugging
    nixpkgs.gdb
  ];

  # LIBCLANG_PATH = "${nixpkgs.libclang.lib}/lib";
  # LIBVNCSERVER_HEADER_FILE = "${nixpkgs.libvncserver.dev}/include/rfb/rfb.h";
}
