# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

This is **Pastime**, a long-lived fork of RetroArch that delivers a MinUI-style experience (no config, folder-based content, minimalist launcher UX) on modern Android handhelds. Upstream is `libretro/RetroArch`. Read **`PLAN.md`** before starting non-trivial work — it owns the architectural strategy, the roadmap, and the list of allowed upstream patch points.

The codebase is otherwise still RetroArch: cores are loaded dynamically as shared libraries implementing `libretro.h` (`libretro-common/include/libretro.h`); the runloop, drivers, and libretro ABI are unchanged.

## Patch discipline (Pastime-specific)

Maintainability of the fork depends on keeping our delta against upstream small and legible. When making changes:

- **New code goes in `pastime/` or `menu/drivers/pastime.c`.** Both are Pastime-owned; edit freely.
- **Edits to any other file are "patch points"** and are tightly controlled. The current allowed set is enumerated in `PLAN.md` ("Allowed patch points"). Adding a new patch point is a deliberate decision — pause and consider whether the work belongs in `pastime/` or the menu driver instead.
- **Every line of patched upstream code gets a marker comment**: `/* PASTIME: <one-line rationale> */`. `git grep PASTIME` should enumerate the fork delta in upstream files. The marker survives rebases and tells the next reader why the line is different.
- **Defaults**: do not edit `config.def.h` to change Pastime defaults. Add to `pastime_defaults_apply()` instead, called once after `config_load()` inside `retroarch_main_init()` — i.e., after upstream defaults *and* on-disk config have been applied, but before CLI args override.
- **Rebase, don't merge.** This fork tracks upstream master via rebase. Avoid commits that would be painful to rebase (sweeping reformatting, accidental upstream-file edits).

## Build

The build system is a hand-rolled `./configure` + GNU make (scripts in `qb/`), not autotools. There is no `cmake`, no `meson`, and no `npm`-style task runner.

- Configure & build (desktop):
  - `./configure` — see `./configure --help` for the full list of `--enable-*` / `--disable-*` / `--with-*` toggles. Generates `config.mk` + `config.h`.
  - `make -jN` — primary desktop build. Resulting binary: `./retroarch`.
  - `make clean` to wipe object files.
- Platform builds use a per-platform Makefile, e.g. `make -f Makefile.win`, `Makefile.emscripten`, `Makefile.ctr`, `Makefile.libnx`, `Makefile.vita`, `Makefile.wiiu`, `Makefile.ps2`, `Makefile.griffin`, `Makefile.apple`, `Makefile.msvc`, etc. `Makefile.common` is shared object/source rules included by the platform makefiles.
- "Griffin" build (`griffin/griffin.c` and friends) is a single-translation-unit unity build used for some console targets — when adding new `.c` files used on those platforms, ensure they get picked up by `griffin.c` or the appropriate platform Makefile.
- macOS / iOS / tvOS: use the Xcode projects under `pkg/apple/` (or `Makefile.apple` for command-line builds).
- Android (primary Pastime target): `pkg/android/build-and-install.sh [debug|release]` is the daily iteration loop. It runs `mise exec -- ./gradlew assembleAarch64Debug` (mise activates JDK 11 from `pkg/android/phoenix/.mise.toml`), `adb install -r`s the APK, and `adb shell am force-stop gg.pastime.aarch64`s the running process so the new code actually loads. Set `ANDROID_SERIAL` if multiple devices are attached. The Android build uses `ndk-build` via `pkg/android/phoenix-common/jni/Android.mk`, *not* `Makefile.common`. The Pastime applicationId is `gg.pastime.aarch64` (and `.ra32` / bare `gg.pastime` for the other flavors), so it installs alongside any pre-existing stock RetroArch (`com.retroarch.aarch64`) without signature conflicts.

## Tests

**Pastime-owned code has unit tests** under `pastime/tests/`. Run them with `bash pastime/tests/run_tests.sh` — each binary builds standalone via `cc` against the real production source (no carbon copies), with a small `PASTIME_*_TEST_BUILD` define swapping heavy RA dependencies (verbosity, HTTP/IO, etc.) for test stubs. Tests use a tiny in-file framework (`ASSERT_TRUE` / `ASSERT_EQ` / `RUN_TEST`); add new tests here as new Pastime-owned modules land. Coverage today: `pastime_nav.c` (full), `pastime_display_name.c`, `pastime_metadata_disambig.c` (resolver — table + fallback semantics), `pastime_thumbs.c` (index parse + match cascade — the pure side; the manager's HTTP/queue/IO is stubbed out via `PASTIME_THUMBS_TEST_BUILD`). The async manager paths (HTTP downloads, file I/O, gfx_thumbnail integration, WebP decode) are intentionally not unit-tested; they're verified end-to-end on Android via `pkg/android/build-and-install.sh`.

The **upstream RA codebase** has no unit tests. `tests-other/` contains integration-style assets (`.ratst` replay/test files, `.cfg` input fixtures) used to exercise input/replay paths — those are run by external CI infrastructure, not via a `make test` target. Don't invent a `make check` invocation for upstream code; if you need to verify changes, build and run `./retroarch -v` against a core.

## Architecture overview

RetroArch is mostly C89-compatible C with a thin C++/Obj-C/Obj-C++ veneer for some UI drivers. The big-picture model:

- **Frontend ↔ libretro core boundary.** `dynamic.c/h`, `core.h`, and `libretro-common/include/libretro.h` define the ABI. Everything in this repo is "the frontend"; the core is loaded as a `.so`/`.dll`/`.dylib` and called through libretro callbacks.
- **Top-level orchestration.** `retroarch.c` (very large) owns init/teardown and global state. `runloop.c` drives the per-frame loop (input poll → core_run → audio/video flush). `command.c` dispatches user/RPC commands. `configuration.c` + `config.def.h` define every user-facing setting and its default; new options must be added in both places (and usually in the menu setting list too).
- **Driver subsystems.** Each I/O concern is a "driver" — an interface struct of function pointers with multiple backend implementations selected at runtime:
  - `gfx/` — video. Backends in `gfx/drivers/` (gl, gl1, gl3, vulkan, d3d10/11/12, metal, etc.), windowing/context in `gfx/drivers_context/`, fonts in `gfx/drivers_font_renderer/`, shaders in `gfx/drivers_shader/`.
  - `audio/` — audio output and DSP.
  - `input/` — input drivers, keyboard/joypad mapping, autoconfig, overlay.
  - `camera/`, `location/`, `led/`, `midi/`, `bluetooth/`, `record/` — minor driver subsystems following the same shape.
  - `menu/` — the in-app menu. `menu_driver.c` is the dispatcher; `menu/drivers/` contains the renderers (XMB, Ozone, MaterialUI/glui, RGUI). `menu/cbs/` holds per-action callbacks; `menu_setting.c` and `menu_displaylist.c` build the setting trees.
  - `ui/drivers/` — the *desktop* UI companion (Cocoa for macOS/iOS in `ui_cocoa*.{m,h}`, Win32, and **Qt** in `ui_qt*.{cpp,h}` / `ui_qt_widgets.*`). The Qt frontend is the "WIMP UI" / playlist browser, separate from the in-app `menu/`.
  - `frontend/` — platform entry points and lifecycle (`frontend/drivers/` per OS).
- **Tasks.** `tasks/` contains the async task system used for background work (downloads, scans, screenshot, autosave, content loading). New long-running operations should be tasks, not blocking calls in the runloop.
- **libretro-common.** `libretro-common/` is a vendored copy of shared libretro utility code (file I/O, formats, compat shims, networking, encodings, streams). **Prefer its APIs over Qt/POSIX equivalents when touching cross-platform code** — the recent Qt cleanup commits have been moving in that direction.
- **Database / playlists.** `libretro-db/` is a small key/value DB used for content metadata; `playlist.c` and `core_info.c` build on top of it. `database_info.c` glues them to the menu.

### Pastime-owned code

Pastime's delta lives in two places — both safe to edit freely:

- `menu/drivers/pastime.c` — the launcher itself, registered as the `"pastime"` menu driver. Single-file driver; navigation is mode-driven via `enum pastime_view` (`PASTIME_VIEW_TOP` / `SYSTEM` / `RECENTS` / `INGAME` / `SAVE_PICKER` / `SETTINGS` / `CONFIRM`). The `frame` callback dispatches to a per-view renderer; `entry_action` (overrides `generic_menu_entry_action`) handles input. Settings views use a small stack-based push/pop model so submenus (Frontend, Emulator, Save Changes…) don't need their own view enums.
- `pastime/` — self-contained C modules called from a small number of upstream patch points:
  - `pastime_bootstrap.{c,h}` — first-run: ensure `Pastime/{Roms,Bios,Saves,States}` exist and seed `Roms/README.txt`.
  - `pastime_defaults.{c,h}` — defaults overlay applied after `config_load()` (sets `menu_driver = "pastime"`, paths, save/state UX flags, session-only persistence, Vulkan-on-Android, gamepad menu combo, etc.). Also owns `pastime_paths_get_root` (Android storage-tier walker → `Pastime/` root).
  - `pastime_cores.{c,h}` — buildbot list cache + sequential install queue. Powers both the boot splash and the lazy "core not installed at launch time" fallback.
  - `pastime_setup.{c,h}` — first-run sequential bucket downloader (core info, assets, joypad autoconfigs, databases, overlays, slang shaders). Reuses RA's online-updater download + decompress task plumbing.
  - `pastime_nav.{c,h}` — pure helpers for the menu driver's navigation model (view stacks, focus persistence, list-cursor math). Heavily unit-tested.
  - `pastime_display_name.{c,h}` — folder-name → display-name normalization (the "Display Name (core_ident)" convention strip + presentation rules).
  - `pastime_metadata.{c,h}` + `pastime_metadata_disambig.c` — per-system metadata index (cached labels, art-state flags) + the disambiguation resolver that maps a Pastime folder name to a canonical thumbnails/db system name. Filename-driven; the old CRC/RDB matching path was removed (commit 8ac1f15c94).
  - `pastime_thumbs.{c,h}` — custom thumbnail manager replacing RA's playlist/gfx_thumbnail download path. Per-system `index.json.gz` from `thumbnails.pastime.gg`, deterministic two-phase match cascade, bounded image fetch queue with active/prefetch priorities, atomic on-disk caching. See the file header for the design contract.
  - `pastime_webp.{c,h}` — vendored libwebp wrapper for synchronous WebP decode into a `gfx_thumbnail_t`. Used when the thumbnail index reports a `.webp` is available (smaller than `.jpg` at indistinguishable quality).

The single source of truth for what is and isn't a sanctioned modification is **`PLAN.md`'s "Allowed patch points" table**.

## Conventions specific to this codebase

The full rules are in `CODING-GUIDELINES`; the ones that bite most often:

- **C89-compatible C.** Declare variables at the top of a function or block — no mid-block declarations, no `for (int i = ...)`, no VLAs. The XBox 360 / MSVC build enforces this. Code must also compile as ISO C++ (some platforms compile `.c` as C++).
- **Allman braces.** Single-statement blocks should not use braces (unless a multi-line macro requires them). Prefer `for (;;)` over `while (true)`.
- **No `-Wall` warnings.** Treat warnings as bugs.
- **Avoid trivial getters/setters.** A function should justify its call overhead; one-liners over POD structs are discouraged. Inline access is preferred.
- **Struct member ordering matters.** Order by alignment (largest first), and keep pointer/length pairs adjacent for cacheline locality. See `CODING-GUIDELINES` for the full type-size table.
- **Stack budget is tight.** Some console targets have ~128 KB stacks. Don't put `char path[PATH_MAX_LENGTH]` arrays on the stack casually; balance against heap fragmentation.
- **Adding a setting** typically means: declare in `configuration.h`, default in `config.def.h`, parse/serialize in `configuration.c`, expose in `menu_setting.c` / `menu_displaylist.c`, add a translation key in `intl/` and `msg_hash*` files.
- **Adding a source file** that compiles on console targets: also wire it into `griffin/griffin.c` (or `griffin_cpp.cpp` / `griffin_objc.m`) and the relevant platform `Makefile.*`, not just `Makefile.common`.
- **Translations** live in `intl/` and are managed via Crowdin — don't hand-edit non-English `msg_hash_*.h` files.

## Working dirs

The repo is checked out in two places: `/Users/nchapman/Drive/Code/Pastime` (primary) and `/Users/nchapman/Code/Pastime`. Default to the primary unless told otherwise.

## When in doubt

- **Read `PLAN.md`** for the project's intent, milestone targets, and patch-point boundaries.
- **Read `CODING-GUIDELINES`** for upstream RetroArch's C89 / brace / struct-ordering rules. They apply to Pastime code too.
- **Prefer the menu driver.** If a feature can be implemented in `menu/drivers/pastime.c` instead of by patching upstream code, do that.
