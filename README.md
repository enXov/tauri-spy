# tauri-spy

**Enable DevTools in Tauri release builds.**

`tauri-spy` is a CLI tool that launches any Tauri application with its developer tools (inspector) enabled — even in production/release builds where DevTools are normally disabled.

## How It Works

1. You run `tauri-spy <path-to-tauri-app>`
2. The tool injects `libspy.so` via `LD_PRELOAD`
3. The injection library hooks `gtk_main()` and enables developer extras
4. DevTools become available via **Ctrl+Shift+I** or **right-click → Inspect Element**

## Installation

### From Source

```bash
# Prerequisites (Debian/Ubuntu)
sudo apt install libwebkit2gtk-4.1-dev libgtk-3-dev pkg-config gcc

# Build
cargo build --release

# Binary at: target/release/tauri-spy
# Library at: target/release/libspy.so
```

## Usage

```bash
# Launch a Tauri app with DevTools enabled
tauri-spy /path/to/tauri-app

# Auto-open the inspector on launch
tauri-spy --auto-open /path/to/tauri-app

# Pass arguments to the target app
tauri-spy /path/to/tauri-app -- --some-flag value
```

## Support Matrix

| Platform       | Architecture | Status         |
| -------------- | ------------ | -------------- |
| Linux          | x86_64       | ✅ Supported    |
| Linux          | aarch64      | 🔲 Untested    |
| macOS          | -            | ❌ Not supported |
| Windows        | -            | ❌ Not supported |

> **Note**: macOS uses WKWebView and Windows uses WebView2 — different injection techniques would be needed. Contributions welcome!

## License

[MIT](LICENSE)
