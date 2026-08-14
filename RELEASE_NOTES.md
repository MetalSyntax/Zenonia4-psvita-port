# Zenonia 4 PS Vita Port v1.0.0 — Turbo Release (Multi-Tier FrameSkip & No-Logs Max Performance)

**Release Tag:** `v1.0.0-turbo`

---

## ⚔️ Zenonia 4 — PlayStation Vita Port (v1.0.0 Turbo Edition)

First official release of the native **Zenonia 4 (Android)** port for the **PlayStation Vita**, featuring **intelligent multi-tier FrameSkip**, **direct RGB565 hardware rendering**, **RAM audio caching**, and **maximum CPU optimization**.

---

## 🚀 What's New in v1.0.0 Turbo

- **⚡ Multi-Tier FrameSkip (1.0x to 3.0x)**:
  - Dynamic frame pacing allowing you to adjust game speed in real-time between **1.0x (Normal)**, **1.5x**, **2.0x (Default)**, **2.5x**, and **3.0x (Max)**.
  - Game logic, physics, enemy AI, and touch/pad inputs run at a full **60 Hz tick rate** while rendering load is cut down, yielding a super-smooth 20–30+ FPS experience.
  - Frame pacing is active across gameplay, Title screen animations, and Main Menus.
- **🖥️ On-Screen OSD HUD Notification**:
  - Displays a clean, translucent banner at the top of the screen when changing speed modes (`FRAME SKIP: 2.0X`, etc.) with smooth fade-out.
- **🛡️ 100% Authentic Visuals & UI Bug Fixes**:
  - Off-screen text and button caches (`p9 != 0`) are fully preserved, fixing missing text labels on pause/inventory popups (*Destroy*, *Repair*, *Equip*, *Discard*).
  - Redundant 1.04 MB `FastClearNonClipping` writes are bypassed on skipped frames to drastically reduce RAM bandwidth usage.
- **🔇 Zero-Log Max CPU Performance**:
  - Release binaries are built with file logging disabled (`ENABLE_LOGGING=OFF`), eliminating SD card / SD2Vita write latency and format-string overhead during gameplay.
- **🎵 Multi-Core Thread Affinity**:
  - Main game loop pinned to **CPU Core 0** at 444 MHz.
  - Audio mixing and background OGG playback isolated to **CPU Core 1** with zero SD card access (audio cached in RAM).

---

## 🎮 Controls

| Button / Input | Action |
| :--- | :--- |
| **D-Pad / Left Stick** | Move character / Navigate menus |
| **Cross (✕)** | Attack / Confirm / Interact |
| **Circle (◯)** | Back / Cancel |
| **Triangle (△)** | Skip dialogue / Cutscene |
| **Square (□)** | Open World Map |
| **L-Trigger** | Quick Save Game |
| **R-Trigger + SELECT** | **Cycle FrameSkip Speed (1.0x ➔ 1.5x ➔ 2.0x ➔ 2.5x ➔ 3.0x)** |
| **START + SELECT** | Exit Game safely |
| **Front Touchscreen** | Full native touch controls & menu navigation |

---

## 📦 Installation Instructions

1. Install `zenonia_4_turbo_nologs.vpk` on your PlayStation Vita via **VitaShell**.
2. Extract the game asset files from the Android APK (`assets/` and `lib/armeabi/libgameDSO.so`) into `ux0:data/zenonia-4/`.
3. Ensure `ux0:data/zenonia-4/libgameDSO.so` and `ux0:data/zenonia-4/assets/` are present.
4. Launch **Zenonia 4** from LiveArea and enjoy the game!

---

## 📁 Downloads / Assets

- **`zenonia_4_turbo_nologs.vpk`** *(Recommended)*: Maximum CPU speed, zero SD card I/O.
- **`zenonia_4_turbo.vpk`**: Turbo variant with active telemetry/debug logging.
- **`zenonia_4.vpk`**: Standard build with Multi-Tier FrameSkip.
