{
  inputs = {
    flake-utils.url = "github:numtide/flake-utils";
    nixpkgs.url = "github:nixos/nixpkgs/release-25.05";
    git-ignore-nix = {
      url = "github:hercules-ci/gitignore.nix/master";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };
  outputs =
    { self
    , flake-utils
    , nixpkgs
    , git-ignore-nix
    , ...
    }:
    flake-utils.lib.eachDefaultSystem (system:
    let
      # like lib.lists.remove, but takes a list of elements to remove
      llvmVersion = "20";
      removeFromList = toRemove: list: pkgs.lib.foldl (l: e: pkgs.lib.remove e l) list toRemove;
      picomOverlay = final: prev: {
        picom = prev.callPackage ./package.nix {
          inherit git-ignore-nix;
          llvmPackages = prev."llvmPackages_${llvmVersion}";
        };
      };
      overlays = [
        picomOverlay
      ];
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
            "llvmPackages_${llvmVersion}" = prev."llvmPackages_${llvmVersion}" // {
              stdenv = final.withCFlags "-fno-omit-frame-pointer" prev."llvmPackages_${llvmVersion}".stdenv;
            };
          })
        ];
      };

      mkDevShell = p: p.overrideAttrs (o: {
        hardeningDisable = [ "fortify" ];
        shellHook = ''
          # Workaround a NixOS limitation on sanitizers:
          # See: https://github.com/NixOS/nixpkgs/issues/287763
          export LD_LIBRARY_PATH+=":/run/opengl-driver/lib"
          export UBSAN_OPTIONS="disable_coredump=0:unmap_shadow_on_exit=1:print_stacktrace=1"
          export ASAN_OPTIONS="disable_coredump=0:unmap_shadow_on_exit=1:abort_on_error=1"
        '';
      });
    in
    rec {
      overlay = picomOverlay;
      packages = {
        picom = pkgs.picom;
        default = pkgs.picom;
      } // (nixpkgs.lib.optionalAttrs (system == "x86_64-linux") rec {
        picom-cross = {
          armv7l = pkgs.pkgsCross.armv7l-hf-multiplatform.picom;
          aarch64 = pkgs.pkgsCross.aarch64-multiplatform.picom;
          i686 = pkgs.pkgsi686Linux.picom;
          merged = pkgs.runCommand "picom-merged" {} ''
            mkdir $out
            ln -s ${picom-cross.armv7l} $out/armv7l
            ln -s ${picom-cross.aarch64} $out/aarch64
            ln -s ${picom-cross.i686} $out/i686
          '';
        };
      });
      devShells.default = mkDevShell (packages.default.override { devShell = true; });
      devShells.useClang = devShells.default.override {
        inherit (pkgs."llvmPackages_${llvmVersion}") stdenv;
      };
      # build picom and all dependencies with frame pointer, making profiling/debugging easier.
      # WARNING! many many rebuilds
      devShells.useClangProfile = (mkDevShell (profilePkgs.picom.override { devShell = true; })).override {
        stdenv = profilePkgs.withCFlags "-fno-omit-frame-pointer" profilePkgs."llvmPackages_${llvmVersion}".stdenv;
      };
    });
}
