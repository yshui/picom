{
  asciidoctor,
  dbus,
  docbook_xml_dtd_45,
  docbook_xsl,
  llvmPackages,
  lib,
  libconfig,
  libev,
  libGL,
  libepoxy,
  libX11,
  libxcb,
  makeWrapper,
  meson,
  ninja,
  pcre2,
  pixman,
  pkg-config,
  python3,
  stdenv,
  uthash,
  xcbutil,
  xcbutilimage,
  xcbutilrenderutil,
  xorgproto,
  xwininfo,
  withDebug ? false,
  withDocs ? false,
  withTools ? false,
  git-ignore-nix,
  devShell ? false,
}:

let
  versionFromMeson =
    s: builtins.head (builtins.match "project\\('picom',.*version: *'([0-9.]*)'.*" s);
in
stdenv.mkDerivation (finalAttrs: {
  pname = "picom";
  version = versionFromMeson (builtins.readFile ./meson.build);

  src = git-ignore-nix.lib.gitignoreSource ./.;

  strictDeps = true;

  nativeBuildInputs = [
    makeWrapper
    meson
    ninja
    pkg-config
  ]
  ++ (lib.optional devShell [
    llvmPackages.clang-tools
    llvmPackages.clang-unwrapped.python
    llvmPackages.libllvm
    (python3.withPackages (
      ps: with ps; [
        xcffib
        pip
        dbus-next
        pygit2
      ]
    ))
  ])
  ++ (lib.optional withDocs [
    asciidoctor
    docbook_xml_dtd_45
    docbook_xsl
  ]);

  buildInputs = [
    dbus
    libconfig
    libev
    libGL
    libepoxy
    libX11
    libxcb
    pcre2
    pixman
    uthash
    xcbutil
    xcbutilimage
    xcbutilrenderutil
    xorgproto
  ];

  # Use "debugoptimized" instead of "debug" so perhaps picom works better in
  # normal usage too, not just temporary debugging.
  mesonBuildType = if withDebug then "debugoptimized" else "release";
  dontStrip = withDebug;

  mesonFlags = [
    (lib.mesonBool "with_docs" withDocs)
  ];

  installFlags = [ "PREFIX=$(out)" ];

  # In debug mode, also copy src directory to store. If you then run `gdb picom`
  # in the bin directory of picom store path, gdb finds the source files.
  postInstall =
    lib.optionalString withTools ''
      wrapProgram $out/bin/picom-trans \
        --prefix PATH : ${lib.makeBinPath [ xwininfo ]}
    ''
    + lib.optionalString withDebug ''
      cp -r ../src $out/
    '';
})
