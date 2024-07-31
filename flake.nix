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
      # like lib.lists.remove, but takes a list of elements to remove
      removeFromList = toRemove: list: pkgs.lib.foldl (l: e: pkgs.lib.remove e l) list toRemove;
      overlay = self: super: {
        picom = super.picom.overrideAttrs (oldAttrs: rec {
          version = "11";
          pname = "picom";
          nativeBuildInputs = (removeFromList [ self.asciidoc ] oldAttrs.nativeBuildInputs) ++
            [
              self.asciidoctor
            ];
          buildInputs =
            [
              self.pcre2
              self.xorg.xcbutil
              self.libepoxy
            ] ++ (removeFromList [
              self.xorg.libXinerama
              self.xorg.libXext
              self.pcre
            ]  oldAttrs.buildInputs);
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
          (final: prev: {
            llvmPackages_18 = prev.llvmPackages_18 // {
              stdenv = final.withCFlags "-fno-omit-frame-pointer" prev.llvmPackages_18.stdenv;
            };
          })
        ];
      };

      overlays = [overlay];
      mkDevShell = p: p.overrideAttrs (o: {
        nativeBuildInputs = o.nativeBuildInputs ++ (with pkgs; [
          clang-tools_18
          llvmPackages_18.clang-unwrapped.python
          llvmPackages_18.libllvm
          python
        ]);
        hardeningDisable = ["fortify"];
        shellHook = ''
          # Workaround a NixOS limitation on sanitizers:
          # See: https://github.com/NixOS/nixpkgs/issues/287763
          export LD_LIBRARY_PATH+=":/run/opengl-driver/lib"
          export UBSAN_OPTIONS="disable_coredump=0:unmap_shadow_on_exit=1:print_stacktrace=1"
          export ASAN_OPTIONS="disable_coredump=0:unmap_shadow_on_exit=1:abort_on_error=1"
        '';
      });
    in rec {
      inherit
        overlay
        overlays
        ;
      packages = {
        default = pkgs.picom;
      };
      devShells.default = mkDevShell packages.default;
      devShells.useClang = devShells.default.override {
        inherit (pkgs.llvmPackages_18) stdenv;
      };
      # build picom and all dependencies with frame pointer, making profiling/debugging easier.
      # WARNING! many many rebuilds
      devShells.useClangProfile = (mkDevShell profilePkgs.picom).override {
        stdenv = profilePkgs.withCFlags "-fno-omit-frame-pointer" profilePkgs.llvmPackages_18.stdenv;
      };
    });
}
