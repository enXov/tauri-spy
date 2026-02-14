use std::env;
use std::path::PathBuf;
use std::process::Command;

fn get_pkg_config_cflags(lib: &str) -> Vec<String> {
    let output = Command::new("pkg-config")
        .args(&["--cflags", lib])
        .output()
        .unwrap_or_else(|_| panic!("Failed to run pkg-config for {} — is pkg-config installed?", lib));

    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr);
        panic!(
            "pkg-config --cflags {} failed: {}",
            lib, stderr
        );
    }

    String::from_utf8_lossy(&output.stdout)
        .trim()
        .split_whitespace()
        .map(|s| s.to_string())
        .collect()
}

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

    // Get include flags from pkg-config for WebKitGTK and GTK3
    let mut cflags: Vec<String> = Vec::new();
    cflags.extend(get_pkg_config_cflags("webkit2gtk-4.1"));
    cflags.extend(get_pkg_config_cflags("gtk+-3.0"));

    // Deduplicate flags
    cflags.sort();
    cflags.dedup();

    // Compile spy.c into libspy.so
    let mut gcc_args: Vec<String> = vec![
        "-shared".to_string(),
        "-fPIC".to_string(),
        "-o".to_string(),
        output.to_str().unwrap().to_string(),
        spy_c.to_str().unwrap().to_string(),
    ];
    gcc_args.extend(cflags);
    gcc_args.extend([
        "-ldl".to_string(),
        "-Wall".to_string(),
        "-Wextra".to_string(),
        "-O2".to_string(),
    ]);

    let status = Command::new("gcc")
        .args(&gcc_args)
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
