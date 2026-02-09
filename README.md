# Simple DX11 FPS Limiter & Overlay

A lightweight, open-source FPS Limiter and Overlay for DirectX 11 games. 
Designed with compatibility in mind, it works natively on Windows and is fully compatible with **macOS (CrossOver/Whisky)** and **Linux (Wine/Proton)** environments.

![Screenshot Placeholder](docs/screenshot.png)
*(You can add a screenshot here later)*

## Features

*   **FPS Limiter**: Precise framerate control to reduce heat, noise, and frame variance.
*   **Background Throttling**: Automatically lowers FPS when you Alt-Tab (saves battery/resources).
*   **Overlay HUD**:
    *   Real-time Framerate graph.
    *   System Clock.
    *   Session Timer (Playtime).
    *   Widgets positionable in the top-right corner.
*   **GUI Launcher**: 
    *   Manage multiple game profiles.
    *   Auto-detects game icons.
    *   Minimizes to tray logic (auto-hide on launch).
*   **Mac/Linux Friendly**: 
    *   Dedicated keybinds (`Shift+Tab`, `Backspace`) for keyboards without Numpad/Function keys.
    *   Robust hooking (MinHook) that survives resizing and Alt-Tabbing in Wine wrappers.

## Installation

1.  Download the latest release from the [Releases Page](../../releases).
2.  Extract the archive (`x64` for 64-bit games, `x86` for 32-bit games).
3.  Run `FPSLauncher.exe`.
4.  Add your game executable (`.exe`).
5.  Configure per-game settings and click **Launch**.

## Building from Source

### Prerequisites
*   **CMake** (3.10+)
*   **MinGW-w64** (gcc)
*   **Make**

### Build Commands (Cross-compilation from macOS/Linux)
```bash
# Build x64
mkdir build_x64 && cd build_x64
cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/x86_64-w64-mingw32.cmake ..
make

# Build x86
mkdir build_x86 && cd build_x86
cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/i686-w64-mingw32.cmake ..
make
```

## Credits

*   **MinHook** by TsudaKageyu - API Hooking library.
*   **ImGui** by ocornut - Bloat-free Graphical User interface.
*   **DirectX 11** - Microsoft Corporation.

## License

MIT License. See [LICENSE](LICENSE) for details.
