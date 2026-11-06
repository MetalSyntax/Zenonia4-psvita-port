# Zenonia 4 PS Vita Port v1.1.0 — Original Game Speed, Options Menu & Remapping

**Release Tag:** `1.1.0`

---

## 🚀 What's New in v1.1.0

- **⏱️ Correct game speed.** Zenonia 4 was designed to run at **21 FPS in menus and 23 FPS in gameplay** (the Android game asks for it itself). Earlier builds ignored that, so menus ran too fast, combat ran in slow motion and the fixed frame-skip patterns made characters stutter while walking. The game now always runs at its original speed.
- **🎞️ AUTO frame skip (default).** It skips drawing a frame only when the game falls behind, so movement speed stays constant even in heavy fights. OFF draws every frame, but the game slows down in heavy scenes.
- **⚡ Optional 30 FPS mode** (Zenonia 2/3 style): the whole game runs about 1.3x faster.
- **🎮 PS Vita options menu (START + SELECT)**, ported from the Zenonia 3 port: remap every action, change FPS, show or hide the on-screen buttons. The game pauses while it's open.
- **👆 Hide the on-screen buttons (R + L).** They still respond to touch, and the game runs noticeably faster with them hidden.
- **🕹️ Left stick moves**, and input no longer lags or gets stuck (all queued button events are now handled every frame).
- **🖼️ Faster framebuffer upload.** Down from ~17 ms to ~1.6 ms per frame (3-texture ring instead of vitaGL's copy-on-write).

## ⚙️ Defaults

| Setting | Default | Change with |
| :--- | :--- | :--- |
| Frame skip | **AUTO** | R + SELECT (AUTO / OFF) |
| FPS | **23** (original speed) | L + SELECT (23 / 30) |
| On-screen buttons | **Shown** | R + L |
| Button layout | Cross = Attack/OK, Circle = Back, Triangle = Skip, Square = Map, L = Save | START + SELECT |

On-screen messages only appear when you change a setting. Settings are saved in `ux0:data/zenonia-4/config.txt` and `controls.txt`.

## ⚠️ Known Limitations

- **Heavy combat shows ~10–12 frames per second while the game logic stays at the full 23** (with AUTO, hidden buttons give 10–14). The original engine draws everything in software on a single CPU thread, and one drawn frame costs ~70 ms on the Vita.
- **START + SELECT no longer exits the game** — use the PS button.

## 📦 Installation

1. Install `zenonia_4_pace_nologs.vpk` with **VitaShell**.
2. Copy `libgameDSO.so` (from the APK's `lib/armeabi/`) and the APK's `assets/` folder into `ux0:data/zenonia-4/`.
3. Launch **Zenonia 4** from LiveArea.

## 📁 Downloads

- **`zenonia_4_pace_nologs.vpk`** *(recommended)*: no logging, no SD card writes.
- **`zenonia_4_pace.vpk`**: same build with logging to `ux0:data/zenonia-4/logs/log_NNNN.log` (for bug reports).
