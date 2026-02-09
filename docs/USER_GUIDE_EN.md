# User Guide

FPS Limiter & Overlay allows you to cap frame rates in DirectX 11 games to ensure frame pacing smoothness and reduce hardware load. It also provides a customizable overlay HUD.

## Quick Start

1.  **Launch**: Open the folder matching your game architecture (`x64` or `x86`) and run `FPSLauncher.exe`.
2.  **Add Game**:
    *   Click the `+ Add` button.
    *   Select your game's executable (`.exe`).
    *   The game will appear in the list with its icon.
3.  **Configuration**:
    *   Select the game from the list.
    *   **Enable Limit**: Toggles the FPS cap.
    *   **Target FPS**: Set your desired framerate (e.g., 60).
    *   **Background Limit**: Lowers FPS when you Alt-Tab out of the game (saves power).
    *   **Overlay**: Toggle `Show Clock` or `Session Timer` (playtime tracker).
    *   **Toggle Key**: Assign a hotkey to open the in-game menu (Default: `Shift+Tab`).
    *   **Start Visible**: Uncheck this if you want the overlay menu hidden when the game starts.
4.  **Play**:
    *   Click the blue `LAUNCH GAME` button.
    *   The launcher will minimize, and the game will start with the overlay injected.

## In-Game Controls

*   Press your assigned **Toggle Key** (e.g., `Shift+Tab`) to open or close the settings menu.
*   You can adjust the FPS limit in real-time without restarting the game.
*   Widgets (Clock/Timer) are anchored to the top-right corner of the screen.

## Troubleshooting

**Overlay does not appear:**
1.  Ensure you represent using the correct version (`x64` launcher for 64-bit games).
2.  Some games with Anti-Cheat software may block DLL injection.
3.  If the game crashes, try disabling other overlays (Discord, Steam, NVIDIA).

**Mac/Linux Compatibility:**
This application is tested and optimized for CrossOver, Whisky, and Proton. Keybinds are designed to be Mac-keyboard friendly (using standard combinations instead of Function keys).
