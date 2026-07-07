{
  description = "CoW Development shell";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
      ];

      forAllSystems = nixpkgs.lib.genAttrs systems;
    in {
      devShells = forAllSystems (system:
        let
          pkgs = import nixpkgs {
            inherit system;
          };
        in {
          default = pkgs.mkShell {
            hardeningDisable = [ "fortify" ];
            nativeBuildInputs = with pkgs; [
              bison
              cairo
              flex
              libbsd
              libevent
              libxkbcommon
              meson
              ninja
              pango
              pixman
              pkg-config
              scdoc
              wayland
              wayland-protocols
              wayland-scanner
              xwayland
            ];
          };
        });
    };
}
