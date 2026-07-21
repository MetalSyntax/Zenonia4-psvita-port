# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

**This file describes the CURRENT, ACTIVE state of the port** (updated 2026-07-21, up through
`port_progress.md` Fase 68). For the full phase-by-phase history of every bug found/fixed and every
build variant tried, `port_progress.md` is authoritative — this file is an orientation map, not a
replacement for it.

## What this repository is

This is a **working PS Vita port of Zenonia 4** (Android package `com.gamevil.zenonia4.global`,
version 1.2.3), using the **soloader** approach: a native Vita executable that loads the Android
`.so` directly (no recompilation), resolves its imports against a homemade libc/EGL/pthread/etc.
implementation, and emulates JNI with FalsoJNI. The game **boots, plays, saves/loads, and the story
progresses** — this is well past the "does it even start" stage; current work is fixing GPU-rendering
correctness/performance regressions in an experimental rendering variant (see "Where things stand" below).

**Engine confirmed** (via `nm -D`/Ghidra, not assumed): Gamevil's own in-house engine, **not** cocos2d-x.
Native entry points are under `Java_com_gamevil_nexus2_Natives_*`; internal C++ classes use `CGs*`
(Gamevil System), `CMv*`, `CZn*` (Zenonia-specific), and `GVUI*` (a separate generic Gamevil UI library)
prefixes.

Layout:

- **`loader/` — the ACTIVE soloader source, this is what you edit.** Forked from the `Zenonia3-vita`
  project's `loader/` lineage (same Nexus2/Clet engine as Zenonia 2/3, confirmed by symbol comparison),
  not written from scratch. Key files: `main.c` (boot sequence + render loop), `dynlib.c` (import
  resolution + the bulk of runtime hooks — GL wrappers, `PutCompressImg`/`RGBBlend16`/
  `DrawNativeFillEllipse` hooks, hide-dpad hook), `java.c` (FalsoJNI method tables + GFA font bridge),
  `gpu_quads.c`/`.h` (the GPU-quads rendering pipeline, see below), `sprite_decode.c`/`.h` (native RLE
  sprite decoder), `font.c`/`.h` (stb_truetype-backed GFA font rasterizer), `audio.c` (Tremor/OGG
  mixer), `androidui.c` (legacy Android-UI overlay bridge — mostly unused, see Fase 14), `htmltext.c`/
  `htmlview.c` (in-app HTML help/about viewer), `so_util.c`/`.h` (the ELF loader + inline hooking,
  vendored but game-agnostic), `neon_mem.c` (NEON memcpy/memset, opt-in).
- **`source/`, `extras/` — the ORIGINAL generic soloader boilerplate, abandoned and unused (Fase 2).**
  Still present in the tree in case something in it is worth salvaging, but zero lines of it are
  compiled into the current build. Do not treat it as the starting point — `loader/` is.
- `common/` — `debugScreen.c` (on-screen `debugScreenPrintf()`, vendored).
- `lib/` — vendored support libs: `falso_jni/` (FalsoJNI), `fios/`, `sha1/`, `stb/` (stb_truetype),
  `so_util/` (older copy, superseded by `loader/so_util.c`), `libc_bridge/` (a separate CMake
  subproject building `SceLibcBridge`, used when `USE_SCELIBC_IO` is on), `kubridge/` (prebuilt kernel
  bridge stub libs). Also holds `libgameDSO.so` itself (symlinked/copied from the extracted APK) —
  the actual game binary the loader loads at runtime.
- `zenonia-4-1-2-3/` — the extracted original APK (decompiled resources, not source you edit) —
  reference material for `nm`/`strings`/jadx analysis.
- `decompiled/` — `apk_jadx/` (jadx decompile of the Java side) and
  `libgameDSO_armeabi/ghidra/out_ghidra.c` (Ghidra decompile of the native engine, ~7MB — **the
  primary source of truth for "how does the engine actually do X"**, used throughout the GPU_QUADS
  investigation to find real function names/signatures instead of guessing).
- `ux0_data/` — local staging mirror of what goes on the Vita's memory card under `ux0:data/`
  (symlinked assets + sound files), used by `manage_vita.py`/deploy scripts.
- `porting_tools/` — build/deploy/decompile/automation scripts. `build_and_install.sh` and
  `manage_vita.py` have been adapted for this project specifically (see "porting_tools" below); the
  rest are still templates reused from earlier, different ports (Dungeon Hunter 2, Prince of Persia
  Classic) — read before running.
- `psvita-port-toolkit/` — a generic, game-agnostic porting guide and Claude Code skill
  (`skills/psvita-porting/`) distilled from those earlier ports. Written for cocos2d-x engines —
  Zenonia 4 is not cocos2d-x, so treat its phase structure as generically applicable but verify any
  concrete symbol name against this binary, not the guide's examples.
- `port_progress.md` — **the authoritative, chronological log of this port**, from Fase 0 (planning)
  through the current GPU-rendering arc (Fase 43+). Every real bug found, every hypothesis tried and
  confirmed/refuted, every build variant measured. Read the tail of it before assuming something is
  broken/unfixed — it may already be documented there.
- `gpu_rendering_log.md` — an earlier, narrative (Spanish, non-technical-audience) summary of the
  GPU-quads rendering work through roughly Fase 55. **Superseded by `port_progress.md`'s later
  phases** (Fase 56+ found and corrected real inaccuracies in it — e.g. it claims a "solución
  definitiva" that later phases showed still had open bugs). Kept for context but don't treat its
  conclusions as current status.
- `optimization_ideas.md`, `plan_zenonia4_port.md` — planning documents from earlier phases, mostly
  of historical interest now that the ideas in them have been tried (see `port_progress.md` for
  results).

## Where things stand (as of Fase 68)

The port is playable end-to-end: boots, renders, takes touch input, plays audio, saves/loads. The
current active development effort is **Variante 2, `--gpu-quads-test`**
(`build/zenonia_4_gpu_quads_test.vpk`) — offloading the engine's software-rendered sprites to GPU
quads via vitaGL for a real, measured FPS win, while chasing down graphical regressions the offload
introduced. The recurring bug family in this arc: several engine draw paths (sprite blits, text
glyphs, solid-color UI fills, rounded-rect corners) write **directly into the software framebuffer**,
bypassing the one function (`PutCompressImg`) originally hooked to divert content to GPU quads — so
anything using one of those paths gets visually buried under GPU-quad content drawn in the same frame,
regardless of the engine's real intended order. Each instance found so far got the same fix: hook the
real underlying function, convert its draw call into a quad, enqueue it into the *same* ordered quad
queue as sprites. Confirmed instances: text (`CGxFACharCache::DrawToFrameBuffer`), solid alpha fills
(`Screen_RGBBLEND16_Real`), rounded-corner fills (`DrawNativeFillEllipse`, gated to real rounded-rect
corners only). **Still open**: an in-game-only variant of the missing-background bug (item pickup /
save / tip dialogs during actual gameplay, as opposed to the pause menu, which works) whose exact cause
is still being isolated via console-log diagnostics; map character/portal icons not rendering; a
narrower text-cache-staleness report in one pause-menu popup. See `port_progress.md` Fase 63 onward for
the detailed, current diagnosis-in-progress.

Other build variants exist for performance experimentation (see `build.sh`'s own header comment for
the full list with rationale): `--neon-fallback`, `--sprite-decode-test`, `--fastclear-neon-test`,
`--downsample-test`, `--primitives-opt-test`, several `--*-probe`/`--*-test` diagnostic-only builds.
The **default build** (`./build.sh`, no flag) is the stable baseline: `RGB565_DIRECT_UPLOAD=ON`
(confirmed real FPS win, no color regressions), everything else off. GPU_QUADS is NOT on by default —
it's an experimental variant, opt in via `--gpu-quads-test` explicitly.

## Build

Requires the `VITASDK` env var pointing at a VitaSDK install (`-softfp` variant). Two ways to build:

- **`./build.sh [flag]`** (repo root) — the real build script, handles the `/tmp` staging workaround
  for the space in this repo's own path (`vita-pack-vpk` breaks on paths with spaces), sets every
  CMake option explicitly per variant (cache-stickiness gotcha, see the script's own comments), and
  copies the resulting `.vpk` to `build/`. Run with no argument for the default build
  (`build/zenonia_4.vpk`), or a flag like `--gpu-quads-test` for a variant — see the script's header
  comment for the full annotated list.
- **`porting_tools/build/build_and_install.sh`** — interactive wrapper around `build.sh`: presents a
  menu of every flag `build.sh` supports (parsed from `build.sh` itself, so it never goes stale),
  optionally installs+launches in Vita3K on macOS afterward, and archives the unsigned ELF alongside
  the VPK for `vita-parse-core` crash symbolization. Portable to macOS's stock bash 3.2.

Useful `cmake` options (all in `CMakeLists.txt`, normally set via `build.sh`'s per-flag argument
lists, not passed directly): `ENABLE_VERBOSE_JNI_LOG`, `HIDE_VIRTUAL_GAMEPAD`, `RGB565_DIRECT_UPLOAD`
(default ON), `GPU_QUADS`, `SPRITE_DECODE_OPTIMIZE`, `DOWNSAMPLE_RENDER` (+ `DS_NUM`/`DS_DEN`), and a
handful of `INSTRUMENT_*`/diagnostic-only options — see the option() calls in `CMakeLists.txt` for the
full list with descriptions (each documents its own rationale/status inline).

There is no separate lint step; `porting_tools/tests/run_tests.sh` is a *template* for a host-side
pre-build test harness from a different, earlier port, not wired into this build.

### Deploying / getting logs off the console

`porting_tools/manage_vita.py` (menu-driven FTP tool, adapted for this project): option 1 now scans
`build/` for every `.vpk` present and lets you choose which to upload (handles the multiple build
variants above — no longer a single hardcoded path), option 2 downloads the newest crash dump + log
from the console. Edit `VITA_IP`/`VITA_PORT`/`VITA_DATA_DIR` at the top if the console's address or
data path changes.

## Architecture (boot sequence)

`main()` (`loader/main.c`) drives, in order: load/relocate/resolve-imports/patch/flush-caches/init the
`.so` (`loader/so_util.c` + `dynlib.c`), install runtime hooks (`zenonia_install_hide_dpad_hook`,
`zenonia_install_hotpath_probe` — which also installs the always-on `PutCompressImg` crash guard and,
under `GPU_QUADS`, the text/fill/ellipse quad hooks — `zenonia_install_cgsgraphics_probe`,
`zenonia_install_fastclear_optimize`, `zenonia_install_sprite_decode_optimize`), `gl_preload()`/GL
context setup, then manually call `JNI_OnLoad` followed by the engine's real native lifecycle entry
points (`Java_com_gamevil_nexus2_Natives_NativeInitDeviceInfo`/`NativeInitWithBufferSize`/
`NativeRender`/`NativeResize`/`NativeResumeClet`/`handleCletEvent` — resolved directly via `so_symbol`,
not through the JNI table) before entering the render loop.

`loader/java.c` sets up FalsoJNI's method tables (name → ID, ID → implementation) that both
`resolve_imports` and the manually-resolved native entry points call back into for "Java"-side
behavior — this is where the GFA (font) bridge lives (`Zenonia_GFA_*`, backed by `loader/font.c`'s
stb_truetype rasterizer), along with save/load, device-info getters, and Tapjoy/IAB/analytics no-ops.

`loader/dynlib.c` is the biggest file (~2100 lines): the import-resolution table plus essentially all
runtime GL/engine hooks — `glDrawArrays_wrapper`/`glColorPointer_wrapper`/etc. (GL_FIXED→float
conversion, engine GL-state shadowing, the vertex-color-array whitening fix), the RGB565 direct-upload
path, `PutCompressImg_hook` (the sprite-diversion point + a crash guard for an invalid `param_9`), and
— under `GPU_QUADS` — `DrawToFrameBuffer_hook` (text), `Screen_RGBBLEND16_Real_hook` (solid fills),
`DrawNativeFillEllipse_hook` (rounded corners). `loader/gpu_quads.c`/`.h` holds the actual GPU-quad
cache/queue/compositing machinery these hooks feed into (`gpu_quads_enqueue_text`/`_fill`,
`gpu_quads_lookup_and_enqueue`/`_finish_decode` for sprites, `gpu_quads_flush`).

## Adapting `porting_tools/`

Most scripts under `porting_tools/` were written for a **different, prior port** (Dungeon Hunter 2 or
Prince of Persia Classic) and hardcode that project's paths/names — read each one before running it.
Two have already been adapted for Zenonia 4 specifically this session (see above): `build_and_install.sh`
and `manage_vita.py`. The rest, still templates:

- **`porting_tools/build/deploy_and_launch_vita3k.sh`** — hardcodes a Vita3K install path and a log
  filename tied to Dungeon Hunter 2's title ID; depends on `porting_tools/automation/click_helper.py`
  with a screen-coordinate offset measured for that project's window layout.
- **`porting_tools/build/decompile_all.sh`** — hardcodes paths for a `Dungeon-Hunter-2-vita` sibling
  directory; point it at this repo, `zenonia-4-1-2-3.apk`, `zenonia-4-1-2-3/lib` before running.
- **`porting_tools/tests/run_tests.sh`** — a host-side test harness for Prince of Persia Classic's
  audio path; not applicable as-is.
- **`porting_tools/build/clean_macos.sh`**, **`porting_tools/misc/get_dump.sh`** — safe to use as-is
  (generic macOS junk-file cleanup / FTP core-dump fetch, no hardcoded project specifics).
- **`porting_tools/automation/*.py`** — generic Quartz-based synthetic mouse/keyboard injection for
  Vita3K's Qt UI; reusable as-is (`pip install pyobjc`).
- **`porting_tools/misc/translate_docs.py`** — generic Markdown translator; requires
  `pip install deep-translator`.

## This game's specifics (confirmed by analysis, not assumed)

- Single native library: `libgameDSO.so` (~1.6 MB, armeabi only).
- Data/assets use Gamevil's own proprietary formats (`.zt1` text tables/dialogue, `.pzx` packed
  resources) — served as opaque bytes via `readAssets`, no custom parser needed in the loader. No
  `.obb`; assets ship inside the APK's own `assets/` tree, mirrored locally under `ux0_data/`.
- Save files: `option.sav`/`SaveN.dat` under `ux0:data/zenonia-4/saves/` — the engine passes a real
  JNI `jbyteArray` (a `JavaDynArray*` in FalsoJNI, not a raw Dalvik-layout pointer — a real, previously
  save-corrupting bug, see Fase 12) for save data, but a raw offset-based struct for `readAssets`
  results — these are two *different* conventions, do not conflate them if touching `saveFile`/
  `loadFile`/`readAssets` again.
- Tapjoy/GCM/IAB are confirmed no-op-safe (native-side symbols exist in the `.so` but nothing calls
  back into "Java" expecting a real backend).
- Rendering is entirely software (a CPU-composited framebuffer uploaded as one texture per frame) with
  several distinct direct-framebuffer-write code paths beyond the obvious sprite blitter — see "Where
  things stand" above; assume there may be more not yet found if a new "renders but element X is
  missing/misordered under GPU_QUADS" symptom appears.

## Toolchain expectations

VitaSDK (`-softfp` variant), `kubridge` (kernel plugin, hardware only), `libshacccg.suprx` present on
the console, a vitaGL build pinned against the installed vitasdk-softfp. **The vitasdk-gcc in current
use is GCC 15.2**, which (since GCC 14) promotes several old-style-C warnings to hard errors by
default — `CMakeLists.txt` downgrades the real ones (`incompatible-pointer-types`, `int-conversion`,
`implicit-function-declaration`, `implicit-int`) back to warnings for the sake of vendored code written
for older compilers. Do not add a new `-Wno-error=X` without confirming `X` is a real warning name for
*this* GCC — an invented one (e.g. `return-mismatch`, which doesn't exist and was found breaking the
build for an unknown stretch of time, see Fase 57) silently aborts compilation for every file, not just
the one that prompted it. Watch for spaces in the project path (`"PSVITA Develop"` breaks
`vita-pack-vpk`) — `build.sh` already stages in `/tmp` to route around this; keep that workaround if
scripting the build further.

## Git hygiene note

`.gitignore` has a broad `lib/` + `*.so` rule under an old "Exclude copyrighted files" comment,
left over from an earlier Zenonia 3 port. In this repo, `lib/` is not just extracted Android
libraries — it also holds the soloader's own vendored source, which is tracked despite the broad
pattern via the files already being force-added in the initial commit. Be careful with any blanket
`git add`/`git clean` in `lib/` — check `git status` first rather than assuming the ignore pattern is
harmless.
