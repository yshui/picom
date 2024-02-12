{
  description = "picom tracer";
  inputs.fenix = {
    inputs.nixpkgs.follows = "nixpkgs";
    url = github:nix-community/fenix;
  };
  inputs.rust-manifest = {
    flake = false;
    url = "https://static.rust-lang.org/dist/2024-02-12/channel-rust-nightly.toml";
  };

  outputs = { self, nixpkgs, fenix, ... } @ inputs: let
    system = "x86_64-linux";
    pkgs = import nixpkgs { inherit system; overlays = [ fenix.overlays.default ]; };
    rust-toolchain = (pkgs.fenix.fromManifestFile inputs.rust-manifest).withComponents [
      "rustfmt"
      "rust-src"
      "clippy"
      "rustc"
      "cargo"
    ];
    makeLinuxHeaders = kernel: pkgs.stdenv.mkDerivation {
      name = "linux-source-${kernel.version}";
      phases = [ "installPhase" ];
      installPhase = ''
        mkdir -p $out/include
        kpath=${kernel.dev}/lib/modules/${kernel.version}
        cp -rv $kpath/source/include/ $out
        chmod +w -R $out
        cp -rv $kpath/build/include/* $out/include
        chmod +w -R $out
        cp -rv $kpath/source/arch/x86/include/* $out/include
        chmod +w -R $out
        cp -rv $kpath/build/arch/x86/include/generated/* $out/include
      '';
    };
    linuxHeaders = makeLinuxHeaders pkgs.linuxPackages_latest.kernel;
  in {
    packages.${system}.rust-toolchain = rust-toolchain;
    devShells.${system}.default = pkgs.stdenv.mkDerivation (with pkgs; {
      name = "picom-tracer";
      nativeBuildInputs = [ rust-toolchain bpftool cargo-generate bcc python311 linuxHeaders pkg-config libbpf clang ];
      buildInputs = [ elfutils zlib ];
      shellHook = ''
        export PYTHONPATH="$PYTHONPATH:${bcc}/lib/python3.11/site-packages/bcc-${bcc.version}-py3.11.egg"
        export BCC_KERNEL_SOURCE="${linuxHeaders}"
        export RUSTFLAGS="-L native=${libbpf}/lib -l elf -l z"
      '';
    });
  };
}
