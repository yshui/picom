{
  inputs = {
    flake-utils.url = github:numtide/flake-utils;
    nixpkgs.url = github:nixos/nixpkgs;
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
      python = pkgs.python3.withPackages (ps: with ps; [
        xcffib pip dbus-next
      ]);

      pkgs = import nixpkgs {
        inherit system overlays;
        config.allowBroken = true;
      };
      profilePkgs = import nixpkgs {
        inherit system;
        overlays = overlays ++ [
          (final: prev: {
            stdenv = prev.withCFlags "-fno-omit-frame-pointer" prev.stdenv;
          })
        ];
      };

      overlays = [overlay];
      mkDevShell = p: p.overrideAttrs (o: {
        nativeBuildInputs = o.nativeBuildInputs ++ (with pkgs; [
          clang-tools_17
          llvmPackages_17.clang-unwrapped.python
          python
        ]);
        hardeningDisable = ["fortify"];
        shellHook = ''
          # Workaround a NixOS limitation on sanitizers:
          # See: https://github.com/NixOS/nixpkgs/issues/287763
          export LD_LIBRARY_PATH+=":/run/opengl-driver/lib"
          export UBSAN_OPTIONS="disable_coredump=0:unmap_shadow_on_exit=1"
          export ASAN_OPTIONS="disable_coredump=0:unmap_shadow_on_exit=1"
        '';
      });
    in rec {
      inherit
        overlay
        overlays
        ;
      defaultPackage = pkgs.picom;
      devShells.default = mkDevShell defaultPackage;
      devShells.useClang = devShells.default.override {
        inherit (pkgs.llvmPackages_17) stdenv;
      };
      # build picom and all dependencies with frame pointer, making profiling/debugging easier.
      # WARNING! many many rebuilds
      devShells.useClangProfile = (mkDevShell profilePkgs.picom).override {
        stdenv = profilePkgs.withCFlags "-fno-omit-frame-pointer" profilePkgs.llvmPackages_17.stdenv;
      };
    });
}
