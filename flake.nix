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
    defaultPackage = pkgs.picom;
    devShell = defaultPackage.overrideAttrs {
      buildInputs = defaultPackage.buildInputs ++ [
        pkgs.clang-tools
      ];
    };
  });
}
