{
  inputs = {
    flake-utils.url = github:numtide/flake-utils;
    git-ignore-nix = {
      url = github:hercules-ci/gitignore.nix/master;
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };
  outputs = {
    self,
    flake-utils,
    nixpkgs,
    git-ignore-nix,
    ...
  }:
    flake-utils.lib.eachDefaultSystem (system: let
      overlay = self: super: {
        picom = super.picom.overrideAttrs (oldAttrs: rec {
          version = "11";
          pname = "picom";
          buildInputs =
            [
              self.pcre2
              self.xorg.xcbutil
              self.libepoxy
            ]
            ++ self.lib.remove self.xorg.libXinerama (
              self.lib.remove self.pcre oldAttrs.buildInputs
            );
          src = git-ignore-nix.lib.gitignoreSource ./.;
        });
      };

      pkgs = import nixpkgs {
        inherit system overlays;
        config.allowBroken = true;
      };

      overlays = [overlay];
    in rec {
      inherit
        overlay
        overlays
        ;
      defaultPackage = pkgs.picom;
      devShells.default = defaultPackage.overrideAttrs (o: {
        nativeBuildInputs = o.nativeBuildInputs ++ (with pkgs; [
          clang-tools_17
          llvmPackages_17.clang-unwrapped.python
        ]);
        hardeningDisable = ["fortify"];
        shellHook = ''
          # Workaround a NixOS limitation on sanitizers:
          # See: https://github.com/NixOS/nixpkgs/issues/287763
          export LD_LIBRARY_PATH+=":/run/opengl-driver/lib"
        '';
      });
      devShells.useClang = devShells.default.override {
        inherit (pkgs.llvmPackages_17) stdenv;
      };
    });
}
