{pkgs ? import <nixpkgs> {}}:
pkgs.mkShell {
  nativeBuildInputs = with pkgs.buildPackages; [
    bison
    cairo
    flex
    libbsd
    libevent
    libxkbcommon
    ncurses
    pango
    pkg-config
    pixman
    wayland
    wayland-scanner
    wayland-protocols
    xwayland
    meson
    lowdown
    python3
  ];
}
