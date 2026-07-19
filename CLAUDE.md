# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repository is

This is a **PS Vita port project for Zenonia 4** (Android package `com.gamevil.zenonia4.global`,
version 1.2.3), using the **soloader** approach: a native Vita executable that loads the Android
`.so` directly (no recompilation), resolves its imports against a homemade libc/EGL/pthread/etc.
implementation, and emulates JNI with FalsoJNI.

Layout:

- `source/`, `lib/`, `extras/`, `CMakeLists.txt` — the actual Vita soloader project (see
  "Architecture" below). **Currently this is the unmodified upstream boilerplate** — no Zenonia-4
  specific code has been written yet (no references to the game, `gamename` placeholders still in
  `CMakeLists.txt`, `main.c`'s render loop is just `// ... do some initialization` /
  `// ... render call` comments). This is the actual starting point for implementation work.
- `zenonia-4-1-2-3/` — the extracted original APK (decompiled resources, not source you edit) —
  reference material for Fase 1-style analysis (`nm`, `strings`, jadx).
- `porting_tools/` — build/deploy/decompile/automation scripts **reused from earlier, different
  ports** (Dungeon Hunter 2, Prince of Persia Classic). Templates, not ready-to-run — see "Adapting
  `porting_tools/`" below before running any of them.
- `psvita-port-toolkit/` — a generic, game-agnostic porting guide and a Claude Code skill
  (`skills/psvita-porting/`) distilled from those earlier ports.

## Build

Requires the `VITASDK` env var (or `CMAKE_TOOLCHAIN_FILE`) pointing at a VitaSDK install.

```sh
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug   # Debug enables -DDEBUG_SOLOADER logging
make -j$(sysctl -n hw.ncpu)         # produces eboot.bin and so_loader.vpk (VITA_VPKNAME)
```

Useful `cmake -D` cache variables (all defined in `CMakeLists.txt`, currently at generic/placeholder
values — see "What needs to change" below):

- `DATA_PATH` — `ux0:data/<gamename>/`, where the game's `.so` and assets live on the memory card.
- `SO_PATH` — defaults to `${DATA_PATH}main.so`; the actual filename to load (Zenonia 4's is
  `libgameDSO.so`, not `main.so`).
- `SHADER_FORMAT` — one of `GLSL` / `CG` / `GXP`.
- `DUMP_COMPILED_SHADERS` — cache compiled shaders to disk (only meaningful for `GLSL`/`CG`).
- `PSVITAIP` — console/emulator IP, used by the `send`/`send_kvdb`/`dump`/`reboot` custom targets.
- `PARSECOREPATH` — path to `vita-parse-core`'s `main.py`, used by the `dump` target.
- `USE_SCELIBC_IO` (default ON) — use `SceLibcBridge` (built from `lib/libc_bridge/`) for IO/libc.

Custom build targets (require a running VitaShell FTP+command server on `PSVITAIP`, ports 1337/1338):

- `make send` — kills the running app, uploads `eboot.bin` via FTP, relaunches it.
- `make send_kvdb` — same but launches via full path (for apps not yet registered/installed).
- `make dump` — fetches the newest `.psp2dmp` from the console (`extras/scripts/get_dump.sh`) and
  symbolizes it against the just-built ELF with `vita-parse-core`.
- `make reboot` — reboots the console/emulator over the command port.

There is no separate lint step; `run_tests.sh` under `porting_tools/tests/` is a *template* for a
host-side pre-build test harness (see below), not something wired into this CMake build.

## Architecture

The soloader itself (`source/` + `lib/`), executed in the order `main()` → `soloader_init_all()`
(`source/utils/init.c`) drives:

1. `fios_init` (`lib/fios/`) — sets up FIOS2 so the loaded `.so` can do file IO against `DATA_PATH`.
2. Sanity checks: `kubridge` loaded, `SO_PATH` exists on the memory card — both `fatal_error()` out
   with an explicit on-screen message rather than silently crashing.
3. `so_file_load` / `so_relocate` / `resolve_imports` / `so_patch` / `so_flush_caches` /
   `so_initialize` (`lib/so_util/so_util.c` + `source/dynlib.c` + `source/patch.c`) — the actual ELF
   loader: loads the `.so` at `LOAD_ADDRESS` (`0x98000000`, `source/utils/init.c`), relocates it,
   resolves its imported symbols against the `dynlib` table in `source/dynlib.c` (the big file,
   ~1000 lines — this is where most libc/pthread/GL/etc. stub wiring for a new game gets added),
   applies any hand-written binary patches (`source/patch.c`, currently a no-op stub), flushes
   caches, then calls the module's init/constructors.
4. `gl_preload()` (`source/utils/glutil.c`) then, back in `main()`, `JNI_OnLoad(&jvm)` is called
   manually and `gl_init()`/`gl_swap()` drive the render loop — `main()` itself is currently just the
   boilerplate skeleton and needs the actual per-game native entry-point calls (`nativeInit`,
   `nativeRender`, etc. — named differently in this game, see "This game's specifics" below) added
   after `JNI_OnLoad`.
5. `jni_init()` (`source/java.c`, using `lib/falso_jni/`) sets up the fake `JavaVM`/`JNIEnv` that
   `resolve_imports` and the manually-resolved native entry points call into; FalsoJNI's method
   tables (name → ID, ID → implementation) are where JNI callback stubs for this game will need to be
   registered per Fase 3 of the porting guide.

Supporting pieces:

- `source/reimpl/` — from-scratch reimplementations of the bits of Bionic libc/pthread/EGL/mem/
  errno/time64/sys that the loaded `.so` needs and that VitaSDK's own libc doesn't provide
  compatibly, plus `asset_manager.cpp` (Android's `AAssetManager` JNI-facing surface — this is
  usually where the asset-packaging decision from Fase 5 of the porting guide gets implemented).
- `source/utils/` — `init.c` (above), `glutil.c` (GL context/shader setup, vitaGL), `dialog.c`
  (on-screen native error dialogs — `fatal_error()`), `settings.c`/`settings.h` (loader settings
  persisted for a separate `configurator.bin` app, launched via a LiveArea `-config` boot param —
  currently only sample placeholder settings), `logger.c` (file logging, one log per run — the
  workflow described in Fase 6 of the porting guide), `utils.c` (misc helpers e.g. `file_exists`,
  `module_loaded`).
- `lib/so_util/` — the ELF loader itself (vendored, not game-specific).
- `lib/falso_jni/` — FalsoJNI (vendored).
- `lib/fios/`, `lib/sha1/` — vendored support libs.
- `lib/libc_bridge/` — a separate CMake subproject (`add_subdirectory`) building `SceLibcBridge`, a
  stub library bridging to Sony's own libc for functions `USE_SCELIBC_IO` routes through; has its own
  `nids.yml`.
- `lib/kubridge/` — prebuilt kubridge stub libs (`.a`), not source.
- `extras/livearea/` — the 4 required LiveArea PNGs + `template.xml`, baked into the `.vpk` by
  `vita_create_vpk()` in `CMakeLists.txt`. Currently generic/placeholder art — replace with Zenonia 4
  branding when ready (must stay 8-bit indexed PNGs at exact sizes, per Fase 8 of the porting guide).
- `extras/cpuinfo`, `extras/meminfo` — also baked into the `.vpk` root.
- `extras/scripts/get_dump.sh` — used by the `make dump` CMake target; a duplicate of
  `porting_tools/misc/get_dump.sh` (both fetch the newest `.psp2dmp` over FTP) — keep them in sync or
  dedupe if one is edited.

## What needs to change from the boilerplate for Zenonia 4

None of this has been done yet — it's the actual next-step work list, derived from cross-referencing
the CMake/source boilerplate against the confirmed facts in "This game's specifics":

- `CMakeLists.txt`: `VITA_TITLEID`, `VITA_APP_NAME`, `DATA_PATH` (`ux0:data/gamename/` →
  something under `ux0:data/`), `SO_PATH` (must resolve to `libgameDSO.so`, not `main.so`),
  `PSVITAIP` default.
- `source/main.c`: after `JNI_OnLoad`, add the real native lifecycle calls — but this game's engine
  is Gamevil's own (see below), so its native entry points are named `Java_com_gamevil_nexus2_
  Natives_NativeInitWithBufferSize`/`NativeRender`/`NativeResize`/etc., not the cocos2d-x-style names
  the porting guide's examples use.
- `source/dynlib.c`: populate/extend the import-resolution table for whatever `libgameDSO.so`
  actually imports (confirm with `nm -D --undefined-only` on that `.so`, don't assume it matches a
  prior port's table).
- `source/java.c` / FalsoJNI tables: register the `Natives_*` methods and work out which JNI
  callbacks the engine calls back into "Java" for (Tapjoy ads, GCM push are strong no-op candidates —
  see below) — following the no-op-vs-must-fire-callback distinction in Fase 3 of the porting guide.
- `source/reimpl/asset_manager.cpp`: implement against Zenonia 4's actual asset layout (`.zt1`/`.pzx`
  proprietary formats, no `.obb` — see below) once the path-resolution semantics are confirmed
  experimentally.
- `extras/livearea/*`, `template.xml`: replace placeholder art/metadata with Zenonia 4's.

## Where to look for the porting methodology

Do not re-derive the soloader architecture from scratch — it's already documented:

- **`psvita-port-toolkit/PORTING_GUIDE.md`** — phase-by-phase guide (toolchain → APK analysis →
  loader → FalsoJNI → graphics → assets → hardware debugging → input → VPK packaging → checklist).
  Read this first for the overall plan.
- **`psvita-port-toolkit/skills/psvita-porting/references/*.md`** — deep technical detail per topic
  (`so_loading.md`, `jni_bindings.md`, `jni_stubs.md`, `input_handling.md`, `asset_packaging.md`,
  `hardware_debugging.md`, `vpk_packaging.md`, `vitasdk_setup.md`, `toolchain_gotchas.md`, etc.). The
  `psvita-porting` skill surfaces these automatically for porting/crash-triage work.
- The `so-crash-triage` skill is the methodology to use for diagnosing a crash from a console log +
  `.psp2dmp`, cross-referencing the real `.so`, its Ghidra pseudo-C, and the jadx-decompiled Java —
  rather than guessing.

**Important caveat:** the guide and skill were distilled from **cocos2d-x** ports. Zenonia 4 does
**not** use cocos2d-x — see below. Treat the guide's phases (soloader structure, FalsoJNI resolution
order, asset fallback strategy, touch-slot mapping, VPK packaging) as generically applicable, but
don't assume cocos2d-x-specific symbol names (`CCFileUtils`, `CC_MAX_TOUCHES`, `nativeSetPaths`,
`Java_org_cocos2dx_lib_...`) exist in this binary — confirm everything against `libgameDSO.so` with
`nm`/`strings` per Fase 1 of the guide, on this binary, not by assumption from prior ports.

## This game's specifics (already confirmed by analysis)

- Single native library: `zenonia-4-1-2-3/lib/armeabi/libgameDSO.so` (~1.6 MB, armeabi only — no
  armeabi-v7a variant).
- Engine is Gamevil's own proprietary in-house engine (**not** cocos2d-x): exported native entry
  points are under `Java_com_gamevil_nexus2_Natives_*` (`NativeInitWithBufferSize`, `NativeRender`,
  `NativeResize`, `handleCletEvent`, etc.), and internal C++ classes use `CGs*` (Gamevil System),
  `CMv*`, `CZn*` (Zenonia) prefixes — e.g. `CGsTouchMgr`, `CMvApp`, `CZnTouchKeypad`.
- Main Android entry point is `.SkeletonLauncher` (see `AndroidManifest.xml`).
- Uses Tapjoy for ads (`com/tapjoy`, `assets/js/mraid.js`) and Google C2DM/GCM for push — these are
  strong no-op candidates per Fase 3 of the porting guide, but confirm each stub doesn't block on a
  callback the engine actually waits for.
- Game data/assets use Gamevil's own proprietary formats (`.zt1` for text tables/dialogue,
  `.pzx` for packed resources) rather than anything cocos2d-x-standard — don't assume a familiar
  asset pipeline, inspect these per-game.
- No `.obb` present alongside the APK in this repo — assets ship inside `assets/` in the APK itself
  (`assets/data`, `assets/script`, `assets/map`, `assets/char`, `assets/obj`, `assets/font`, per-locale
  dirs `eng/chi/jpn/deu/fra/tai`, etc.). Confirm experimentally (Fase 5 of the guide) how the engine
  actually resolves these paths before choosing an asset-packaging strategy for the Vita build.

## Adapting `porting_tools/`

Every script under `porting_tools/` was written for a **different, prior port** and hardcodes that
project's paths/names. None of them work out of the box for Zenonia 4 — read each one before running
it and update the marked variables:

- **`porting_tools/build/build_and_install.sh`** — hardcodes `BUILD_DIR=/tmp/dh2-build`,
  `SRC_DIR=/tmp/dh2-src`, `VPK_NAME="dungeon_hunter_2.vpk"`, and an `rsync --exclude` list tuned to
  that project's extracted-APK folder names. Rewrite these for this Zenonia 4 tree (which now has a
  real `CMakeLists.txt`/`source/`/`lib/` to build).
- **`porting_tools/manage_vita.py`** — hardcodes `VITA_IP`, `VITA_PORT`, `LOCAL_VPK_PATH`,
  `VITA_DATA_DIR`, `VITA_LOGS_DIR`, `VITA_CG_DIR` for Dungeon Hunter 2's title ID/folder name; also
  shells out to `protonvpn-cli disconnect`, which assumes that tool is installed. Update the constants
  at the top of the file before use.
- **`porting_tools/build/deploy_and_launch_vita3k.sh`** — hardcodes `/tmp/dh2-build`, a Vita3K
  `fs/ux0/app/PSVDH0002` install path, and a log filename tied to Dungeon Hunter 2's title ID; also
  depends on `porting_tools/automation/click_helper.py` and a screen-coordinate offset measured for
  that project's Vita3K window layout.
- **`porting_tools/build/decompile_all.sh`** — hardcodes `BASE_DIR`, `APK_FILE`, and `LIB_DIR` for a
  `Dungeon-Hunter-2-vita` sibling directory. Point it at
  `"/Volumes/Seagate/PSVITA Develop/Zenonia4-vita"`, `zenonia-4-1-2-3.apk`, and
  `zenonia-4-1-2-3/lib` before running (it drives `jadx` for the Java side and
  `devrvk/so-decompiler`/Ghidra via Docker for `libgameDSO.so`).
- **`porting_tools/tests/run_tests.sh`** — a host-side pre-build test harness for Prince of Persia
  Classic's audio path/decoder (`libgame_logic.so`/`libcocosdenshion.so`, `ux0_data/popclassic`). Not
  applicable to Zenonia 4 as-is; treat it as a template for whatever host-testable logic (if any) the
  Vita port ends up needing.
- **`porting_tools/build/clean_macos.sh`** — safe to use as-is; deletes macOS `._*` junk files that
  accumulate from non-HFS+ volumes (this repo lives on `/Volumes/Seagate/...`) and can confuse
  packaging tools if left in a build tree.
- **`porting_tools/misc/get_dump.sh <vita-ip>`** — safe to use as-is; fetches the newest `.psp2dmp`
  core dump from the console over FTP (port 1337, VitaShell's FTP server). Functionally identical to
  `extras/scripts/get_dump.sh`, which is the one actually wired into the CMake `dump` target.
- **`porting_tools/automation/*.py`** (`click_helper.py`, `hold_click.py`, `mousedown_only.py`,
  `mouseup_only.py`, `key_helper.py`) — generic Quartz-based synthetic mouse/keyboard injection for
  automating the Vita3K emulator's Qt UI (which ignores AppleScript/accessibility clicks). Reusable
  as-is; require `pip install pyobjc`.
- **`porting_tools/misc/translate_docs.py`** — generic Markdown batch translator via
  `deep_translator`/Google Translate; requires `pip install deep-translator`.

## Toolchain expectations

Per `psvita-port-toolkit/PORTING_GUIDE.md` Fase 0: VitaSDK (`-softfp` variant), `kubridge` (kernel
plugin, hardware only — `soloader_init_all()` hard-fails at startup if it's not loaded),
`libshacccg.suprx` present on the test console, and — since this project links `vitaGL` — a
version/commit pinned deliberately against the installed vitasdk-softfp. Watch for spaces in the
project path (`"PSVITA Develop"` in this repo's own path breaks `vita-pack-vpk`) — the reused
`porting_tools` build scripts route around this via a `/tmp` build/src staging directory; keep that
workaround (or an equivalent) when driving the real `cmake`/`make` build from automation.

## Git hygiene note

`.gitignore` has a broad `lib/` + `*.so` rule under an "Exclude copyrighted files" comment, left over
from an earlier Zenonia 3 port's `.gitignore` (there are also unrelated `zenonia3`/`zenonia3_java`
entries above it, harmless but stale). In *this* repo, `lib/` is not just extracted Android
libraries — it's also the soloader's own vendored source (`lib/so_util`, `lib/falso_jni`, `lib/sha1`,
`lib/fios`, `lib/libc_bridge`, `lib/kubridge`), which needs to be tracked in git. As-is, `git add`
will silently skip all of it. Fix the `.gitignore` (e.g. scope the exclusion to the extracted-APK
directory only) before the first commit that's meant to include the soloader project.
