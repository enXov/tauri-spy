use clap::Parser;
use colored::Colorize;
use std::env;
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

    Ok(())
}

fn main() -> ExitCode {
    let cli = Cli::parse();

    // Validate target binary
    if let Err(e) = validate_target(&cli.target) {
        eprintln!("{} {}", "error:".red().bold(), e);
        return ExitCode::FAILURE;
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

    // Launch target with LD_PRELOAD
    let status = Command::new(&cli.target)
        .args(&cli.args)
        .env("LD_PRELOAD", &preload)
        .env("TAURI_SPY_AUTO_OPEN", auto_open)
        .status();

    match status {
        Ok(status) => {
            if status.success() {
                ExitCode::SUCCESS
            } else {
                let code = status.code().unwrap_or(1);
                ExitCode::from(code as u8)
            }
        }
        Err(e) => {
            eprintln!("{} Failed to launch target: {}", "error:".red().bold(), e);
            ExitCode::FAILURE
        }
    }
}
