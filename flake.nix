{
  inputs = {
    flake-utils.url = github:numtide/flake-utils;
    git-ignore-nix = {
      url = github:hercules-ci/gitignore.nix/master;
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };
  outputs = {
    self, flake-utils, nixpkgs, git-ignore-nix, ...
  }: flake-utils.lib.eachDefaultSystem (system: let
    overlay = self: super: {
      picom = super.picom.overrideAttrs (oldAttrs: rec {
        pname = "picom";
        buildInputs = [
          self.pcre2 self.xorg.xcbutil
        ] ++ self.lib.remove self.xorg.libXinerama (
          self.lib.remove self.pcre oldAttrs.buildInputs
        );
        src = git-ignore-nix.lib.gitignoreSource ./.;
      });
    };
    pkgs = import nixpkgs { inherit system overlays; config.allowBroken = true; };
    overlays = [ overlay ];
  in rec {
    inherit overlay overlays;
    defaultPackage = pkgs.picom.overrideAttrs (o: {
      version = "11";
      src = ./.;
      buildInputs = o.buildInputs ++ [ pkgs.libepoxy ];
    });
    devShell = defaultPackage.overrideAttrs {
      buildInputs = defaultPackage.buildInputs ++ (with pkgs; [
        clang-tools_17
        llvmPackages_17.clang-unwrapped.python
        libunwind
      ]);
      hardeningDisable = [ "fortify" ];
      shellHook = ''
        # Workaround a NixOS limitation on sanitizers:
        # See: https://github.com/NixOS/nixpkgs/issues/287763
        export LD_LIBRARY_PATH+=":/run/opengl-driver/lib"
      '';
    };
  });
}
