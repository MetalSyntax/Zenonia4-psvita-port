# Zenonia 4 PS Vita Port v1.2.0 — Multi-Core Sprite Rendering

**Release Tag:** `1.2.0`

---

## 🚀 What's New in v1.2.0

- **🧵 Uses all 3 CPU cores for sprite drawing.** Large sprites are split into horizontal bands and drawn on cores 0, 2 and 1 at the same time, producing exactly the same pixels as the original renderer. The game now shows **15–20 FPS in combat (was 10–12)** with AUTO frame skip, and ~17 FPS with frame skip OFF. Game speed stays at the original 23. Audio keeps priority on its core.
- **⚙️ Frame skip in the options menu.** START + SELECT now has Frame skip, Frame rate and On-screen buttons, each with its shortcut shown. The frame skip choice is saved too.

## ⚠️ Known Limitations

- The heaviest scenes can still dip to ~12 FPS.
- All three CPU cores show high usage while playing, which is expected: the helper threads wait for sprites without sleeping, to react instantly. They sleep when the game isn't drawing.
- START + SELECT doesn't exit the game; use the PS button.

## ⚙️ Defaults

| Setting | Default | Change with |
| :--- | :--- | :--- |
| Frame skip | **AUTO** | R + SELECT, or START + SELECT menu |
| FPS | **23** (original speed) | L + SELECT, or START + SELECT menu |
| On-screen buttons | **Shown** | R + L, or START + SELECT menu |
| Button layout | Cross = Attack/OK, Circle = Back, Triangle = Skip, Square = Map, L = Save | START + SELECT |

On-screen messages only appear when you change a setting. Settings are saved in `ux0:data/zenonia-4/config.txt` and `controls.txt`.

## 📦 Installation

1. Install `zenonia_4_pace_nologs.vpk` with **VitaShell** (it updates v1.1.0 in place; your saves and settings are kept).
2. First install only: copy `libgameDSO.so` (from the APK's `lib/armeabi/`) and the APK's `assets/` folder into `ux0:data/zenonia-4/`.
3. Launch **Zenonia 4** from LiveArea.

## 📁 Downloads

- **`zenonia_4_pace_nologs.vpk`** *(recommended)*: no logging, no SD card writes.
- **`zenonia_4_pace.vpk`**: same build with logging to `ux0:data/zenonia-4/logs/log_NNNN.log` (for bug reports).

---

## Previous release: v1.1.0 — Original Game Speed, Options Menu & Remapping

**Release Tag:** `1.1.0`

---

### What was new in v1.1.0

- **⏱️ Correct game speed.** Zenonia 4 was designed to run at **21 FPS in menus and 23 FPS in gameplay** (the Android game asks for it itself). Earlier builds ignored that, so menus ran too fast, combat ran in slow motion and the fixed frame-skip patterns made characters stutter while walking. The game now always runs at its original speed.
- **🎞️ AUTO frame skip (default).** It skips drawing a frame only when the game falls behind, so movement speed stays constant even in heavy fights. OFF draws every frame, but the game slows down in heavy scenes.
- **⚡ Optional 30 FPS mode** (Zenonia 2/3 style): the whole game runs about 1.3x faster.
- **🎮 PS Vita options menu (START + SELECT)**, ported from the Zenonia 3 port: remap every action, change FPS, show or hide the on-screen buttons. The game pauses while it's open.
- **👆 Hide the on-screen buttons (R + L).** They still respond to touch, and the game runs noticeably faster with them hidden.
- **🕹️ Left stick moves**, and input no longer lags or gets stuck (all queued button events are now handled every frame).
- **🖼️ Faster framebuffer upload.** Down from ~17 ms to ~1.6 ms per frame (3-texture ring instead of vitaGL's copy-on-write).
