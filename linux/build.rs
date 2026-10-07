use std::{env, path::PathBuf, process::Command};

fn run(command: &mut Command) {
    let status = command
        .status()
        .expect("run CMake (install cmake and ninja)");
    assert!(status.success(), "native Qt build failed");
}

fn main() {
    println!("cargo:rerun-if-changed=ui");
    let out = PathBuf::from(env::var_os("OUT_DIR").unwrap());
    let native = out.join("native");
    run(Command::new("cmake")
        .args(["-S", "ui", "-B"])
        .arg(&native)
        .args(["-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"]));
    run(Command::new("cmake")
        .arg("--build")
        .arg(&native)
        .args(["--parallel", "2"]));
    println!("cargo:rustc-link-search=native={}", native.display());
    println!("cargo:rustc-link-lib=static=pegline_ui");
    for library in [
        "Qt6Widgets",
        "Qt6Gui",
        "Qt6DBus",
        "Qt6Core",
        "LayerShellQtInterface",
        "KF6GlobalAccel",
        "wayland-client",
        "stdc++",
    ] {
        println!("cargo:rustc-link-lib={library}");
    }
}
