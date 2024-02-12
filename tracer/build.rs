use std::env;
use std::env::consts::ARCH;
use std::ffi::OsStr;
use std::path::Path;
use std::path::PathBuf;

use libbpf_cargo::SkeletonBuilder;

const SRC: &str = concat!(env!("CARGO_MANIFEST_DIR"),"/src/bpf/uprobe.bpf.c");

fn main() {
    let mut out =
        PathBuf::from(env::var_os("OUT_DIR").expect("OUT_DIR must be set in build script"));
    out.push("uprobe.skel.rs");
    SkeletonBuilder::new()
        .source(SRC)
        .clang_args(
            format!("-I{}", Path::new("../vmlinux").join(ARCH).display())
        )
        .build_and_generate(&out)
        .unwrap();
    println!("cargo:rerun-if-changed={SRC}");
    println!("cargo:rustc-link-lib=static=elf");
}
