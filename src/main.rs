use clap::Parser;
use colored::Colorize;
use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode};

/// Enable WebKitGTK DevTools in Tauri release builds
#[derive(Parser)]
#[command(name = "tauri-spy", version, about, long_about = None)]
struct Cli {
    /// Path to the target Tauri application binary
    target: PathBuf,

    /// Automatically open the inspector on launch
    #[arg(long)]
    auto_open: bool,

    /// Additional arguments to pass to the target application
    #[arg(trailing_var_arg = true, allow_hyphen_values = true)]
    args: Vec<String>,
}

fn find_libspy() -> Result<PathBuf, String> {
    // Check next to the current executable first
    if let Ok(exe_path) = env::current_exe() {
        let dir = exe_path.parent().unwrap();

        // Check in same directory as executable
        let candidate = dir.join("libspy.so");
        if candidate.exists() {
            return Ok(candidate);
        }

        // Check in ../lib/ (for installed layouts)
        let candidate = dir.join("../lib/libspy.so");
        if candidate.exists() {
            return Ok(candidate.canonicalize().map_err(|e| e.to_string())?);
        }
    }

    Err("Could not find libspy.so — is it built?".to_string())
}

fn validate_target(path: &Path) -> Result<(), String> {
    if !path.exists() {
        return Err(format!("Target binary not found: {}", path.display()));
    }

    if !path.is_file() {
        return Err(format!("Target is not a file: {}", path.display()));
    }

    // Check if file is executable
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let metadata = path.metadata().map_err(|e| e.to_string())?;
        if metadata.permissions().mode() & 0o111 == 0 {
            return Err(format!("Target is not executable: {}", path.display()));
        }
    }

    // Read ELF header for validation
    let bytes = fs::read(path).map_err(|e| format!("Failed to read target binary: {}", e))?;

    // Check ELF magic bytes
    if bytes.len() < 64 || &bytes[0..4] != b"\x7fELF" {
        return Err(format!(
            "Target is not an ELF binary: {}\n  {} tauri-spy only works with Linux ELF executables",
            path.display(),
            "hint:".yellow().bold()
        ));
    }

    // Check 64-bit (EI_CLASS == 2)
    if bytes[4] != 2 {
        return Err(format!(
            "Target is a 32-bit binary — tauri-spy requires x86_64\n  {} Rebuild the target for x86_64",
            "hint:".yellow().bold()
        ));
    }

    // Check x86_64 architecture (e_machine == 0x3E at offset 18)
    let e_machine = u16::from_le_bytes([bytes[18], bytes[19]]);
    if e_machine != 0x3E {
        return Err(format!(
            "Target architecture is not x86_64 (e_machine=0x{:X})\n  {} tauri-spy currently only supports x86_64",
            e_machine,
            "hint:".yellow().bold()
        ));
    }

    // Check dynamically linked (ET_DYN or ET_EXEC with PT_INTERP)
    let e_type = u16::from_le_bytes([bytes[16], bytes[17]]);
    // ET_EXEC=2, ET_DYN=3 (PIE executables are ET_DYN)
    if e_type != 2 && e_type != 3 {
        return Err(format!(
            "Target is not an executable ELF (type={})\n  {} Expected a dynamically linked executable",
            e_type,
            "hint:".yellow().bold()
        ));
    }

    Ok(())
}

/// Check if WebKitGTK is available on the system
fn check_webkit_available() -> bool {
    Command::new("pkg-config")
        .args(["--exists", "webkit2gtk-4.1"])
        .status()
        .map(|s| s.success())
        .unwrap_or(false)
}

fn main() -> ExitCode {
    let cli = Cli::parse();

    // Validate target binary
    if let Err(e) = validate_target(&cli.target) {
        eprintln!("{} {}", "error:".red().bold(), e);
        return ExitCode::FAILURE;
    }

    // Check WebKitGTK availability
    if !check_webkit_available() {
        eprintln!(
            "{} WebKitGTK 4.1 not found on this system",
            "warning:".yellow().bold()
        );
        eprintln!(
            "  {} Install with: {}",
            "hint:".yellow().bold(),
            "sudo apt install libwebkit2gtk-4.1-dev".dimmed()
        );
        eprintln!(
            "  {} Continuing anyway — injection may still work if the target bundles WebKitGTK",
            "note:".cyan().bold()
        );
    }

    // Find the injection library
    let libspy_path = match find_libspy() {
        Ok(path) => path,
        Err(e) => {
            eprintln!("{} {}", "error:".red().bold(), e);
            return ExitCode::FAILURE;
        }
    };

    println!(
        "{} Launching {} with DevTools enabled",
        "tauri-spy".cyan().bold(),
        cli.target.display().to_string().green()
    );
    println!(
        "{} Injecting {}",
        "       >>>".cyan(),
        libspy_path.display().to_string().dimmed()
    );

    // Build LD_PRELOAD value, preserving any existing preloads
    let mut preload = libspy_path.to_string_lossy().to_string();
    if let Ok(existing) = env::var("LD_PRELOAD") {
        if !existing.is_empty() {
            preload = format!("{}:{}", preload, existing);
        }
    }

    // Set auto-open environment variable for the injection library
    let auto_open = if cli.auto_open { "1" } else { "0" };

    // Launch target with LD_PRELOAD and WebKit rendering workarounds
    let status = Command::new(&cli.target)
        .args(&cli.args)
        .env("LD_PRELOAD", &preload)
        .env("TAURI_SPY_AUTO_OPEN", auto_open)
        // Work around WebKitGTK GPU rendering issues (blank/black window)
        // See: https://github.com/nicbarker/clay/issues/213
        .env("WEBKIT_DISABLE_COMPOSITING_MODE", "1")
        .env("WEBKIT_DISABLE_DMABUF_RENDERER", "1")
        .status();

    match status {
        Ok(status) => {
            if status.success() {
                ExitCode::SUCCESS
            } else {
                let code = status.code().unwrap_or(1);
                eprintln!(
                    "{} Target exited with code {}",
                    "note:".cyan().bold(),
                    code
                );
                ExitCode::from(code as u8)
            }
        }
        Err(e) => {
            eprintln!("{} Failed to launch target: {}", "error:".red().bold(), e);
            ExitCode::FAILURE
        }
    }
}
