# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

**This file describes the CURRENT, ACTIVE state of the port** (updated 2026-07-26, up through
`port_progress.md` Fase 98). For the full phase-by-phase history of every bug found/fixed and every
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

## Where things stand (as of Fase 98)

The port is playable end-to-end: boots, renders, takes touch input, plays audio, saves/loads, at a
**confirmed real ceiling of ~24 FPS for any 100%-CPU rendering approach** (`--skip-blit-test` measured
this with sprite drawing disabled entirely — real gameplay, with sprites actually drawn, lands
somewhere below that). The default build (`./build.sh`, no flag) currently runs ~9-10 FPS and is the
**recommended build for actually playing** — stable, no known crash/hang risk.

**GPU_QUADS (`--gpu-quads-test`) is PAUSED, not just "experimental"** — it is the only approach that's
shown FPS beyond the CPU ceiling in practice (13-15 FPS measured), but it has a **confirmed, still-
unexplained, deterministic crash** (`PC=0x0` inside vitaGL's own `_glTexSubImage2D`, always during the
engine's framebuffer upload — 6 identical `.psp2dmp`, 5 real hypotheses ruled out with hardware data:
draw-call volume, wrong texture bound, texture-count exhaustion, VSync, RGB565 format specifically) AND
has **hung the entire console twice** (not just crashed the app — no `.psp2dmp`, requires a hard
power-cycle): once with `DISABLE_VSYNC+GPU_QUADS` together (never combine these again), and once from a
`glGetIntegerv` diagnostic call added mid-investigation (reverted). Do not build/ship anything under
`GPU_QUADS` without treating it as actively dangerous to the console, not merely buggy — see
`port_progress.md` Fase 72-91 for the full crash-chasing history and Fase 90 for the one genuinely new
lead (real vitaGL source, fetched from GitHub, pins the exact NULL-pointer mechanism — still needs a
crash-safe way to confirm which texture is actually bound at the crash moment).

**`build.sh` now hard-guards against `DISABLE_VSYNC=ON`+`GPU_QUADS=ON` (Fase 95)** — `--gpu-quads-pure-test`
silently carried this exact hang-risk combination since its introduction (its own name suggested a
"safe" isolated variant, which it was not); a fresh test session ran it, didn't hit the hang this time,
and read that as evidence GPU_QUADS was becoming safer — it wasn't, it was the same risk from Fase 81
recurring undetected. Fixed (`--gpu-quads-pure-test` no longer sets `DISABLE_VSYNC`) and hardened: the
script now scans its own `CMAKE_EXTRA_ARGS` for this combination on every invocation and refuses to build
(`exit 1`) unless `I_ACCEPT_CONSOLE_HANG_RISK=1` is set — this catches the combination even in variants
not yet written, not just the two currently known to produce it. **Any previously-built
`build/zenonia_4_gpu_quads_pure_test.vpk` predates this fix and must be rebuilt before further testing —
do not reuse it.** Separately, GPU_QUADS's "textures erased / untextured triangles at sprite positions
after a stage change" symptom (reported same session) is a real, still-undiagnosed bug independent of the
hang risk — the stage-transition cache invalidation (`gpu_quads_invalidate_cache()`, Fase 69) is confirmed
correctly wired, so the bug is elsewhere in the mass-delete-then-redecode window. See `port_progress.md`
Fase 95.

**Open question, unresolved (Fase 98)**: a session with the corrected (`DISABLE_VSYNC=OFF`)
`--gpu-quads-pure-test` produced a log that cuts off abruptly right after a "frame pesado: quads=16384"
dump — the same profile that preceded the deterministic `PC=0x0` crash in Fase 72-91 — with no clean-exit
banner and no next periodic checkpoint. It's unconfirmed whether this was a real crash/hang (which would
mean the crash mechanism survives even with VSync restored — new information) or just a log pulled
mid-session (`game_log()` only fully flushes on clean exit — see `port_progress.md`'s log-flush gotcha).
The session ended before the user could confirm which. **Ask this again before drawing conclusions either
way** if GPU_QUADS work resumes.

Recent work (Fase 83-94, all CPU-side, GPU_QUADS-independent) chased the ~24 FPS ceiling with several
optimizations, all gated behind their own `option()`/build.sh flag, all requiring `SPRITE_DECODE_OPTIMIZE`
except where noted: `SPRITE_DECODE_EX_ALPHA` (native text-blend RLE variant, `--sprite-decode-exalpha-test`,
less-verified), `SPRITE_DECODE_CACHE` (CPU decode cache for repeat sprites, `--sprite-decode-cache-test`,
**the one candidate that changes the approach — avoids redundant work instead of just speeding up the
same work**; measured a real 0% hit rate in combat (Fase 95) traced to a genuine bug, not a lack of
repeat content — `sc_decode_and_populate`'s row-boundary tracking (`sprite_decode.c`) over-advanced by a
full row whenever a row happened to be exactly full (the common case, not the exception, for opaque
rectangular sprites/tiles), pushing its accounting past capacity and silently discarding almost every
real sprite from the cache; fixed Fase 96 — but that fix immediately exposed a **second** real bug in the
same function (consecutive fully-blank rows lost a row of tracking silently, corrupting pixels instead of
invalidating — visible for the first time on hardware precisely because Fase 96 let the cache start
storing/replaying entries at all), fixed Fase 97 with an explicit per-row counter matching the real
decoder's unconditional `dest += stride` semantics exactly. Fase 97's fix raised the real hit rate to
~99% (162-422 hits/frame vs ~1 miss/frame) — which immediately exposed a **third** real bug, structural
this time: `sc_hash`/`sc_find` (`sprite_decode.c`) never included the palette pointer (`pal`) in the
cache key, unlike its twin mechanism `gq_hash` in `gpu_quads.c` (which does, and measures correct high
hit rates in production). Since pixel color depends on `pal[*p_src]` not just `src`, a cache hit replayed
whichever palette first populated an entry regardless of which palette a later draw actually requested —
invisible while the real hit rate was ~0% (Fase 95), but constant visible color corruption ("errores
graficos que hacen injugable el port") once Fase 97 let hits actually happen at scale. Fixed Fase 99 by
adding `pal` to `sc_entry`/the hash/the key comparison, mirroring `gq_hash` exactly. That fix in turn
surfaced a **fourth** issue (Fase 100): the ONLY real invalidation signal for this cache
(`sprite_cache_invalidate()`, `java.c`) fires on `status==19`, but `g_ui_status` starts at `-1` and
stays there until the engine's first real `OnUIStatusChange` — meaning the entire Gamevil splash/logo
phase runs with zero invalidation checkpoints, the worst case for this cache's weak fingerprint
(pointer + 4 sampled bytes + w/h/pal) against a logo animation that likely redecodes new content at a
stable heap address every frame. Reported as "broken/white/black textures from the Gamevil title,
worse than the default build" (the ONLY code difference between default and `--sprite-decode-cache-test`
is this cache, confirmed via `build.sh` diff). Mitigated (not root-cause-confirmed) Fase 100 by
skipping the cache entirely while `g_ui_status == -1` (falls back to the existing no-cache decode
path, zero cost elsewhere, zero benefit lost since that screen is seen once for a few seconds anyway).
**Still pending hardware confirmation that this actually resolves it — four real interventions in a
row in this exact mechanism (Fase 95, 96/97, 99, 100) is reason for skepticism, not confidence, that a
fifth doesn't remain. If this doesn't resolve the reported corruption, reconsider whether
`SPRITE_DECODE_CACHE` is worth continuing to harden given the ~24 FPS ceiling already limits its
upside regardless.**), `TEXT_NATIVE_OPTIMIZE`
(native text blend, `--text-native-optimize-test`, low expected impact — text is called only ~10-18
times/frame). Recompiling the same algorithm in modern ARMv7 C (vs. the original ARMv5) has NOT shown a
measurable FPS gain anywhere it's been tried (sprites, confirmed Fase 85) — the bottleneck is memory-
bandwidth/volume-bound, not codegen-quality-bound. Diagnostic-only additions: `INSTRUMENT_FRAME_PHASES`
(`--frame-phases-probe`, confirmed 90-96% of frame time is inside the engine's own `NativeRender`, only
4-10% outside it — rules out any optimization outside that call), `INSTRUMENT_TEXT_CALLS`
(`--text-calls-probe`), and `INSTRUMENT_CGSGRAPHICS_CALLS` (`--cgsgraphics-probe`, real combat data shows
`DrawFillRect` at only 3-14 calls/frame — 30-150x less frequent than the sprite hot path, so a GPU
offload of solid-color fills is very unlikely to move FPS meaningfully even if built).

**Known bug, fixed Fase 94**: `game_log()`'s internal buffer was 512 bytes; the periodic "frame N alive"
diagnostic line grew past that (silently, over several phases) and got truncated by `vsnprintf` with no
warning — several `avg_*_per_frame` fields (including the sprite-cache hit/miss counters) were silently
missing from every log since Fase 86. Fixed by bumping the buffer to 2048. If you add more fields to that
one line in the future, mind the total length again.

Other build variants exist for performance experimentation (see `build.sh`'s own header comment for
the full list with rationale, ~30 flags total at this point): `--neon-fallback`, `--fastclear-neon-test`,
`--downsample-test` (confirmed dead end — zooms the view, no FPS/visual change), `--primitives-opt-test`,
several `--*-probe`/`--*-test` diagnostic-only builds. GPU_QUADS is NOT on by default and should not be
enabled for anything other than continued crash investigation with full awareness of the console-hang
risk above.

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
