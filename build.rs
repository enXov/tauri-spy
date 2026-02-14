use std::env;
use std::path::PathBuf;
use std::process::Command;

fn main() {
    let out_dir = PathBuf::from(env::var("OUT_DIR").unwrap());
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let spy_c = manifest_dir.join("inject").join("spy.c");

    // Get the target profile output directory (where the final binary goes)
    // OUT_DIR is something like target/release/build/tauri-spy-xxx/out
    // We want to place libspy.so next to the final binary in target/release/
    let target_dir = out_dir
        .ancestors()
        .find(|p| p.ends_with("release") || p.ends_with("debug"))
        .map(|p| p.to_path_buf())
        .unwrap_or_else(|| out_dir.clone());

    let output = target_dir.join("libspy.so");

    // Compile spy.c into libspy.so
    let status = Command::new("gcc")
        .args(&[
            "-shared",
            "-fPIC",
            "-o",
            output.to_str().unwrap(),
            spy_c.to_str().unwrap(),
            "-ldl",
            "-Wall",
            "-Wextra",
            "-O2",
        ])
        .status()
        .expect("Failed to run gcc — is gcc installed?");

    if !status.success() {
        panic!("Failed to compile inject/spy.c into libspy.so");
    }

    println!("cargo:rerun-if-changed=inject/spy.c");
    println!(
        "cargo:warning=libspy.so built at {}",
        output.display()
    );
}
