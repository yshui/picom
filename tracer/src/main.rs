use libbpf_rs::skel::{OpenSkel, Skel, SkelBuilder};
use libbpf_rs::{PerfBufferBuilder, UprobeOpts};
use std::os::unix::ffi::OsStrExt;
use object::read::elf::{Dyn, FileHeader};

mod uprobe {
    include!(concat!(env!("OUT_DIR"), "/uprobe.skel.rs"));
}
use object::Object;
use uprobe::*;
fn handle_lost_events(cpu: i32, count: u64) {
    eprintln!("Lost {count} events on CPU {cpu}");
}
fn handle_event(cpu: i32, data: &[u8]) {
    eprintln!("Got {} bytes of data on {cpu}", data.len());
}
fn main() {
    let mut builder = UprobeSkelBuilder::default();
    let picom_path = std::env::args().nth(1).unwrap();
    let data = std::fs::read(&picom_path).unwrap();
    let file = object::read::elf::ElfFile64::<'_, object::NativeEndian, _>::parse(&*data).unwrap();
    let header = file.raw_header();
    let sections = header.sections(object::NativeEndian, &*data).unwrap();
    let (dyanmic, dynamic_index) = sections.dynamic(object::NativeEndian, &*data).unwrap().unwrap();
    let strings = sections.strings(object::NativeEndian, &*data, dynamic_index).unwrap();
    let mut runpath = None;
    let mut libc_name = None;
    for d in dyanmic {
        if d.is_string(object::NativeEndian) {
            let s = d.string(object::NativeEndian, strings).unwrap();
            let tag = d.d_tag(object::NativeEndian);
            if tag == object::elf::DT_RUNPATH as u64 {
                runpath = Some(s.to_vec());
            }
            if tag == object::elf::DT_NEEDED as u64 && s.starts_with(b"libc.so") {
                libc_name = Some(s.to_vec())
            }
            eprintln!("{} {}", tag, String::from_utf8_lossy(s));
        }
    }
    let runpath = runpath.unwrap();
    let libc_name = libc_name.unwrap();
    let libc_name = std::ffi::OsStr::from_bytes(&libc_name);
    let mut libc_path = None;
    for p in runpath.split(|ch| *ch == b':') {
        let p = std::ffi::OsStr::from_bytes(p);
        let p = std::path::Path::new(p);
        let p = p.join(libc_name);
        if p.exists() {
            libc_path = Some(p);
        }
    }
    let libc_path = libc_path.unwrap();
    eprintln!("{}", libc_path.to_string_lossy());
    builder.obj_builder.debug(true);

    let open_skel = builder.open().unwrap();
    let mut skel = open_skel.load().unwrap();
    let obj = skel.object_mut();
    let xcb_probe = obj.prog_mut("uprobe_xcb_conn").unwrap();
    let _link = xcb_probe.attach_uprobe_with_opts(-1, &picom_path, 0, UprobeOpts {
        retprobe: false,
        func_name: "xcb_connection_probe".to_string(),
        ..Default::default()
    }).unwrap();
    let epoll_probe = obj.prog_mut("uprobe_epoll_wait").unwrap();
    let _link2 = epoll_probe.attach_uprobe_with_opts(-1, libc_path, 0, UprobeOpts {
        retprobe: false,
        func_name: "epoll_wait".to_string(),
        ..Default::default()
    }).unwrap();
    std::thread::park();
}
