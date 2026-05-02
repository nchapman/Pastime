# Downplay Plan

Downplay is a RetroArch fork that delivers a MinUI-style experience — no config, folder-based content, minimalist launcher UX — on modern Android handhelds (and, incidentally, anywhere RetroArch builds).

This document is the working plan. It is meant to be edited as the project evolves.

## Goals

1. **MinUI-style UX**: gamepad-only, two top-level concepts (Recents and Browse), folders mirror what's on disk, no setup screens, fast resume.
2. **Filesystem is the configuration.** A `Downplay/` directory at storage root, with a fixed subfolder layout, is the entire user-facing config surface. No internal databases, scans, playlists to manage, or settings screens for content.
3. **Cores arrive on demand.** Downplay downloads libretro cores from the buildbot the first time they're needed; the user never installs a core manually.
4. **Modern Android handhelds as the primary target**: Retroid, Ayn, Anbernic Win/Android, etc. — devices MinUI proper doesn't serve well.
5. **Maintainable as a long-lived RetroArch fork**: minimize and isolate patches to upstream code so we can keep rebasing on `libretro/RetroArch` master indefinitely.

## Non-goals

- We are **not** rewriting RetroArch's core, runloop, driver model, or libretro ABI.
- We are **not** trying to replace XMB / Ozone / RGUI. They keep working; Downplay is an additional menu driver.
- We are **not** shipping our own cores. We use libretro cores as-is, downloaded from the libretro buildbot.
- We are **not** supporting every RetroArch setting. The Downplay UI exposes a curated subset; advanced users can still drop into XMB/RGUI if we leave that escape hatch in.

## Storage layout & conventions

Downplay's entire content model is defined by directory structure on disk. There is no hidden state.

### Top-level

A `Downplay/` directory at a known storage root. On Android, "storage root" is resolved through RetroArch's three-tier permission model (`frontend/drivers/platform_unix.c:105–109`) — full external storage, scoped app-external, or app-private — picking the first writable tier where `Downplay/` exists or can be created. On desktop, `~/Downplay/` (or an explicit path via config).

```
Downplay/
   Roms/          — game content, organized by system (see below)
   Bios/          — BIOS files; mapped to RA's system_directory
   Saves/         — save files (RA standard layout)
   States/        — save states (RA standard layout)
```

Bootstrap creates these on first launch if missing. RA-specific paths (`saves`, `states`, etc.) get redirected here via the defaults overlay so RetroArch and Downplay agree on locations.

### System folders inside `Roms/`

Each subdirectory of `Roms/` is one "system" in the UI. The folder name is free-form, but **must end with `(<core_ident>)`** where `core_ident` is the short libretro core name (e.g., `snes9x`, `mgba`, `genesis_plus_gx`).

```
Roms/
   Super Nintendo (snes9x)/
   Game Boy Advance (mgba)/
   Sega Genesis (genesis_plus_gx)/
   GBA - hacks (mgba)/             ← multiple folders can target the same core
```

**Folder name parsing rules:**
- Pattern: `^(.+) \(([a-z0-9_]+)\)$`. Capture group 1 is the display name; capture group 2 is the core ident.
- The core ident maps to `<ident>_libretro.{so,dll,dylib}` for filesystem lookup.
- **Folders without a `(corename)` suffix are hidden.** No fallback, no error. Strict convention is part of the appeal.
- **Folders whose core ident isn't in the libretro buildbot's available list are also hidden.** No "broken folder" UX.
- Multiple folders may resolve to the same core; that's a feature (e.g., separate "official" and "hacks" libraries).

### Cores

Cores are not bundled. Downplay queries the libretro buildbot on boot, caches the available-core list, and:
- Hides any system folder whose core isn't on the list.
- For visible folders whose core isn't yet installed locally, downloads on first launch attempt (showing a "Downloading core…" state in the UI).
- A background pre-download pass on boot is a future enhancement (M3+ scope).

### Per-core config defaults

We will eventually ship opinionated default configs per core (input remaps, hotkeys, integer scaling, etc.). Out of scope for the prototype; deferred past M5.

## Architectural strategy

**The menu driver does most of the work.** RetroArch's `menu_driver` interface (see `menu/menu_driver.h:343` for the `menu_ctx_driver_t` struct) is the right seam: a new `menu/drivers/downplay.c` can present a completely different UI without touching the runloop, content loading, or any driver subsystem. Required functions are minimal — `init`, `free`, `frame`, `ident` — and RGUI (`menu/drivers/rgui.c`) is the closest reference for a "from-scratch" driver.

**What the menu driver owns vs doesn't:**
- *Owns:* rendering (via `gfx_display_*` APIs), navigation callbacks, the `entry_action` hook to override entry semantics, and custom per-driver state via `menu_st->driver_data`.
- *Doesn't own:* raw input — the shell's `menu_event()` (`menu/menu_driver.c:5117`) translates input to `enum menu_action` before the driver sees it. The entry data structure (`menu_list_t` / `file_list_t`) is also fixed; drivers manipulate it via `list_*` callbacks rather than bypassing it. Generic action dispatch (`generic_menu_entry_action()`) handles common cases unless the driver overrides via the optional `entry_action` callback.

This means: standard MinUI navigation (up / down / select / back / page) fits cleanly into existing actions. Anything requiring raw gamepad data — custom long-press gestures, chords not already mapped — would require patching `menu_event()` and is flagged as a contingent patch point.

**Anything that doesn't fit the menu-driver abstraction lives in `downplay/`** — a new top-level directory of self-contained modules. Examples: folder→core resolution, first-run bootstrap, defaults overlay, recents adapter. These modules are pure C with narrow public APIs called from a small number of patch points in upstream files.

**Upstream patch points are explicit and few.** Every modification to a file we did not author carries a `/* DOWNPLAY: <one-line rationale> */` marker. The set of patched files is enumerated below and should grow only with deliberate justification. When upstream changes a patched file, the marker makes the conflict obvious and the rationale survives the rebase.

### Allowed patch points (initial set)

| File | Purpose | Expected size |
|---|---|---|
| `menu/menu_driver.c` (~L331) | Register `&menu_ctx_downplay` in the `menu_ctx_drivers[]` table under `#if defined(HAVE_DOWNPLAY)`. | ~3 lines |
| `menu/menu_driver.h` | `extern menu_ctx_driver_t menu_ctx_downplay;` declaration. | 1 line |
| `Makefile.common` | Compile `downplay/*.o` and `menu/drivers/downplay.o`. The Android NDK build picks these up automatically via the griffin.c unity build. | <10 lines |
| `pkg/android/phoenix-common/jni/Android.mk` (L89–162) | Define `HAVE_DOWNPLAY=1` so the conditional includes activate. | 1 line |
| `retroarch.c` (~L7497, after `config_load()`) | Call `downplay_defaults_apply()` to overlay defaults *after* upstream defaults + on-disk config but *before* CLI override (second pass starts ~L7509). | 1–2 lines |
| `retroarch.c` `retroarch_main_init()` (immediately after the `downplay_defaults_apply()` call) | Call `downplay_bootstrap()` once. Defaults are now in place, paths resolve correctly, drivers haven't been initialized yet. | 1–2 lines |
| `tasks/task_core_updater.c` (~L511, `cb_task_core_updater_download`) and `tasks/tasks_internal.h` | Add a setter `task_core_updater_set_download_callback(retro_task_callback_t)` so Downplay can hook "core finished installing → launch the pending ROM". The existing callback is hardcoded; this is the smallest additive change that avoids polling. | ~10 lines |
| `pkg/apple/rebuild-assets.sh` | Overlay `downplay/assets/*` (e.g. `assets/downplay/InterTight-Bold.ttf`) into the macOS `.app`'s `assets.zip` before zipping, so the bundled-assets pipeline ships our font. | ~6 lines |

**Contingent (only if proven necessary):**

| File | Trigger | Notes |
|---|---|---|
| `menu/menu_driver.c` `menu_event()` (~L5117) | If MinUI-style input requires gestures or button mappings outside the standard `enum menu_action` set. | The driver only receives translated actions (UP, DOWN, OK, CANCEL, etc.); raw input is not exposed to per-driver code. Most MinUI behavior should fit standard actions. |
| `gfx/include/gfx/file_path_special.h` (L142–156) | If we ship our own font/icon asset bundle. | Adding a `APPLICATION_SPECIAL_DIRECTORY_ASSETS_DOWNPLAY` enum value is the conventional way; deferred past prototype. |
| `configuration.c` `config_get_default_menu()` (L1456–1482) | If we need "downplay" to be the *built-in* default menu driver. | Avoidable by setting `settings->arrays.menu_driver = "downplay"` in `downplay_defaults_apply()`. Prefer the overlay route. |

If a feature seems to require patching outside this list, that is a signal to redesign — either push the logic into the menu driver, expose what we need via an upstream PR, or accept a feature gap. New patch points get added to this table with rationale.

### `downplay/` module layout (target)

```
downplay/
   downplay.h                 — shared types, public API surface
   downplay_paths.c           — locate Downplay/ root across Android storage tiers; cache subpaths
   downplay_bootstrap.c       — first-run: ensure Downplay/{Roms,Bios,Saves,States} exist
   downplay_defaults.c        — defaults overlay (menu_driver, system_dir, savefile_dir, etc.)
   downplay_systems.c         — parse Roms/ subfolders: "Display Name (core_ident)" → struct
   downplay_cores.c           — buildbot list cache, installed check, lazy download orchestration
   downplay_recents.c         — thin adapter over content_history playlist
menu/drivers/
   downplay.c                 — the menu driver itself
```

`downplay_systems.c` and `downplay_cores.c` together implement the storage convention. The menu driver consults them; it doesn't parse folder names or talk to the updater directly.

## Roadmap

The roadmap is a sequence of derisking milestones. Each milestone is a working, runnable build. Desktop comes before Android because the iteration loop is much faster and the derisking questions are platform-independent.

### M0 — Hello menu driver *(derisks: build + registration plumbing)* ✅

- [x] Create skeleton `menu/drivers/downplay.c` implementing the bare minimum of the `menu_ctx_driver_t` interface: `init`, `free`, `frame`, `ident`. The placeholder draw goes inside `frame` (no separate `render` slot is required).
- [x] Register it in `menu_driver.c` and add it to `Makefile.common`.
- [x] Renders a placeholder screen — solid background, "Downplay" label.
- [x] Build on macOS desktop, select Downplay as the menu driver via config, confirm it loads.

**Exit criterion:** `./retroarch -v` boots into the Downplay menu driver and renders the placeholder. *(met by `efe25422a8`)*

### M1 — Storage discovery + folder browse *(derisks: storage convention + rendering)* 🚧

- [ ] Implement `downplay_paths.c`: locate `Downplay/` at storage root. On desktop, look at `~/Downplay/`. On Android, walk the three storage tiers from `frontend/drivers/platform_unix.c:105–109,1810–1867` and pick the first writable one with a `Downplay/` directory (or the first writable one at all, for bootstrap). *(Deferred. Currently we read `settings->paths.directory_menu_content` (RA's `rgui_browser_directory`) and let the user point it at `~/Downplay/Roms`. Factoring into `downplay_paths.c` lands with M5/M6 when Android needs it.)*
- [x] Implement `downplay_systems.c`: enumerate `Roms/` via `dir_list_new()`. Parse each subfolder name with the `^(.+) \(([a-z0-9_]+)\)$` rule. Folders without a parens suffix are dropped. Returns a list of `{ display_name, core_ident, path }`. *(Lives inline in `menu/drivers/downplay.c` for now; extract to its own module when a second consumer appears. Also drops empty folders so the launcher only shows rows the user can drill into.)*
- [x] Render the system list as the top of the Browse view; selecting a system reveals its ROMs.
- [x] Use `gfx_display_*` APIs and `gfx_display_font_file()`. Reuse RGUI's font path for the prototype (no custom asset bundle yet). *(Using bundled InterTight-Bold.ttf instead — per the MinUI-style visual direction; falls back to the renderer's built-in font.)*
- [x] Standard navigation only (`MENU_ACTION_UP/DOWN/OK/CANCEL`) via driver navigation callbacks. *(Implemented as an `entry_action` override since we own a single hardcoded list, not a `file_list_t`.)*
- [x] Selecting a ROM still just logs — no launch yet. *(Superseded — actual launch is implemented in M2 below; this milestone's exit criterion is met by reaching the ROM list.)*

**Exit criterion:** drop ROMs into `Downplay/Roms/Super Nintendo (snes9x)/`, see the system and ROMs in the UI. ✅

### M2 — Launch using the parsed core ident *(derisks: content-load path from our driver)* ✅

- [x] Resolve `core_ident` → installed core path: lookup in `core_info_list` (`core_info.h`); if installed, get the local file path. Defer the not-installed case to M3. *(Via `core_info_find("<ident>_libretro", …)`; missing-core case logs and stays put.)*
- [x] On select, call `task_push_load_content_with_new_core_from_menu(core_path, fullpath, &content_info, CORE_TYPE_PLAIN, NULL, NULL)` (`tasks/task_content.c:2312`). Pattern crib: `menu/cbs/menu_cbs_ok.c:2095–2110`.
- [ ] Call `menu_driver_set_last_start_content()` for state consistency. *(Skipped — the helper is `static` in `menu_cbs_ok.c`. Revisit only if its absence shows up as a visible bug; otherwise fold into M7 polish.)*
- [x] Stash any custom driver state in `menu_st->driver_data` before launch. *(Already there — Downplay's handle is the second arg to `init`, which the framework stores as `driver_data`.)*
- [x] Returning from the core lands back in Downplay, not XMB. *(Verified live with gambatte.)*

**Exit criterion:** with snes9x already installed, picking a ROM in `Super Nintendo (snes9x)/` launches and returns cleanly. *(Met live with gambatte/Game Boy.)*

### M3 — Eager core download with lazy fallback *(derisks: offline-availability + the MinUI "magic")* 🚧

**Design shift from the original plan.** Original M3 was lazy-only: download a core the first time the user picks a ROM that needs it. That fails the "I'm on a plane and the core isn't installed" case. Revised M3: download all *referenced* cores up front behind a boot splash; keep a lazy fallback for cores referenced after that splash (folders the user adds mid-session, or a download that errored and was retried later).

The factoring: a single core-installer module (`downplay_cores.c`) with both flows on top of one primitive `downplay_cores_install_one(ident, cb)`. The splash and the lazy-on-demand path are both callers; neither owns the install primitive.

**Module layout — `downplay_cores.c`:**
- `downplay_cores_init()` — kicks off `task_push_get_core_updater_list(list, mute=true, refresh_menu=false)` (`tasks/tasks_internal.h:129`); populates the cached `core_updater_list_t`. Idempotent.
- `downplay_cores_is_installed(ident)` — `path_is_valid` against the resolved `<ident>_libretro.<ext>` in `settings->paths.directory_libretro`.
- `downplay_cores_is_available(ident)` — true iff `ident` is on the cached buildbot list. Returns "unknown" until the list lands so callers can wait.
- `downplay_cores_collect_needed(systems[]) → ident set` — pure function over the parsed system list; no side effects. Reused by the splash *and* by anything that wants to show "missing core" badges. The input is already pre-filtered: `downplay_systems` drops empty folders before this sees them, so we never download a core for a system the user has no games for. (Unique idents — multiple folders can share a core.)
- `downplay_cores_install_one(ident, retro_task_callback_t cb, void *user)` — push `task_push_core_updater_download()` (`tasks/tasks_internal.h:134`) for one ident, dispatch `cb` on success/failure. The single primitive both flows build on.
- `downplay_cores_install_many(idents[], progress_cb, done_cb)` — sequential install (one at a time, simpler progress UI; the buildbot can take the load) by chaining `_install_one` callbacks. Cancellable.

**Flow A — boot splash (primary path):**

- [x] In the menu driver's `init`, after `downplay_rebuild_lists()` runs, call `downplay_cores_begin_boot_setup()` with the parsed system idents. The cores module dedupes, drops already-installed ones, and (only if anything is missing) kicks the buildbot list fetch. The frame stays BLANK while AWAITING_LIST so a "no downloads needed" outcome doesn't briefly flash the splash.
- [x] Splash UI: progress label ("Downloading core…  <ident> (n of m)"), centered. B to cancel (sets the cancelled flag; the in-flight download runs to completion but its result is discarded, then the queue stops).
- [x] On done (success, failure, or cancel) the splash dismisses and the normal TOP view appears.
- [x] Cores referenced by folders but **not** on the buildbot are silently skipped during install. (Visibility filtering — hiding the folder when no core can ever be installed — still TODO; today the launch path logs "core not installed" gracefully.)
- [x] Buildbot-list timeout: if the list fetch hasn't returned within 15 s the splash dismisses anyway, so an offline first boot doesn't strand the user.

**Flow B — lazy fallback (covers gaps):**

- [x] On ROM select where `downplay_cores_is_installed(ident)` is false: stash the pick on the menu handle (`pending_launch_core` / `pending_launch_rom`) and call `downplay_cores_begin_boot_setup(&ident, 1)`.  The same boot splash takes over rendering; `downplay_drive_pending_launch` finishes the launch when the cores state machine returns to LIST mode.  Reusing the splash (rather than building an inline ROM-row pill) keeps the install UX identical to the boot pass.
- [x] B during a lazy install cancels (the in-flight download still runs to completion, but the result is discarded) and clears the pending pick so we don't auto-launch when the task settles.

**Patch point still required:** the additive callback hook in `tasks/task_core_updater.c` (already in the patch table). ✅ landed in `a73829ee32`. The hardcoded `cb_task_core_updater_download` is unchanged; we just gained a single-shot setter for an additional callback.

**System-folder visibility, refined:**
- Until `downplay_cores_init` reports the buildbot list is loaded: show folders whose core is already installed locally, hide everything else.
- Once the list is loaded: also show folders whose core is *available on the buildbot* (the splash will install them).
- Folders whose core is neither installed nor on the buildbot: stay hidden. No "broken folder" UX.

**Why sequential installs:** parallel downloads complicate the cancel UX, fight each other for buildbot bandwidth, and obscure progress. Sequential keeps the splash readable ("3 of 5") and trivial to cancel; the cost is marginal latency since cores are small (a few MB each).

**Exit criterion:** wipe the cores directory; boot Downplay with `Roms/` populated; the splash downloads only the referenced cores (not every buildbot entry); after dismiss, every system folder's ROMs launch without further network. Plus: with the cores directory still empty, kill wifi, boot — splash skips the missing cores after timing out, system folders without an installed core stay hidden, the rest still work.

### M4 — Recents ✅

- [x] Iterate `g_defaults.content_history` via `playlist_size()` + `playlist_get_index()` and cache display rows on drill-in (label, falling back to basename minus extension; rows skipped when both are empty record their original `pl_idx` so the array can't drift from the playlist on launch).
- [x] Render as a drill-in view from the "Recently Played" row (top of the system list when history is non-empty).
- [x] Selecting a recent calls `task_push_load_content_from_playlist_from_menu(entry->core_path, entry->path, entry->label, &content_info, NULL, NULL)`.
- [x] Force `CMD_EVENT_HISTORY_INIT` from `downplay_menu_init` (guarded on `g_defaults.content_history == NULL`). Upstream only fires it lazily on the first content load (`tasks/task_content.c:1643`), so without this the playlist file is never read on a fresh boot and the row never appears.
- [x] Fixed an unrelated double-free at exit (`menu_driver_ctl(RARCH_MENU_CTL_DEINIT)` already frees `menu_st->userdata`; our `free` callback was freeing `dp` again). The crash had been ignored since M0.

**Exit criterion:** Recents reflects play history and re-launches correctly. ✅

### M4.5 — In-game menu 🚧

A MinUI-style overlay shown when the user opens the menu over a running core.  Continue resumes; Quit unloads back to the launcher.  Save/Load/Options come later.

- [x] New `DOWNPLAY_VIEW_INGAME` view, driven from `runloop_get_flags() & RUNLOOP_FLAG_CORE_RUNNING` every frame.  The same condition upstream menu drivers approximate via `MENU_ST_FLAG_PENDING_QUICK_MENU` + `ACTION_OK_DL_CONTENT_SETTINGS`; we read it directly because we don't render the file_list_t stack.
- [x] Consume `MENU_ST_FLAG_PENDING_QUICK_MENU` unconditionally in our pump so `runloop.c:6150` can't queue a displaylist push we'd ignore.
- [x] `prior_view` / `prior_selection` saved on entry, restored on exit (SYSTEM/RECENTS resources stay live during gameplay so no heap copy needed).
- [x] **Continue** → `command_event(CMD_EVENT_MENU_TOGGLE, NULL)`.  **Quit** → `command_event(CMD_EVENT_UNLOAD_CORE, NULL)`.  CANCEL also acts as Continue.
- [x] Background renders with alpha 0.7 in INGAME so the running game shows through.  Title pill (top-left) sources from `path_get(RARCH_PATH_CONTENT)` basename minus extension; sized to the row font, capped at half-screen-minus-margin, ellipsis-truncated when it doesn't fit (UTF-8-safe).
- [x] Generic helpers `downplay_truncate_to_width` and `downplay_draw_text_pill` so future chrome can reuse the size-and-truncate flow.
- [ ] Save / Load / Options.  Out of scope for now.

**Exit criterion:** menu opens over a running core, Continue resumes, Quit returns to the launcher.  ✅ (Continue/Quit only.)

### M5 — Android build + on-device test ✅

- [x] Add Downplay sources + `HAVE_DOWNPLAY` to `pkg/android/phoenix-common/jni/Android.mk`. *(The Android build is ndk-build via `Android.mk`, **not** `Makefile.common` — original plan was wrong about this. Sources are listed explicitly and are guarded by `HAVE_DOWNPLAY ?= 1` for parity.)*
- [x] `HAVE_NETWORKING` already on for the Android build.
- [x] Bump `ndkVersion` from r22 to r26.1: r22 has no Apple-Silicon host, so building on an arm64 Mac requires a newer NDK. Patched in `pkg/android/phoenix/build.gradle` with a `/* DOWNPLAY: */` marker.
- [x] Play Feature Delivery: no code change needed — non-`playStore*` Gradle flavors include `play-core-stub`, which makes `play_feature_delivery_enabled()` return false at runtime, so `task_push_core_updater_download` follows the regular code path.
- [x] Sideload onto a real handheld; storage at `/sdcard/Downplay/`.
- [x] Verified input (built-in controller), rendering, content launching, lazy core download. Initial pass needed two manual config edits (`menu_driver`, gamepad combo) and an `adb push` of the font; all three are superseded by M6.

**Exit criterion:** the desktop M0–M4 experience works on a real handheld. ✅

### M6 — First-run bootstrap ✅

- [x] `downplay_bootstrap()` ensures `Downplay/{Roms,Bios,Saves,States}` exist (idempotent: each `path_mkdir` is gated on `path_is_directory`). Drops a `README.txt` in `Roms/` explaining the `Display Name (corename)` convention; only written when missing so user edits survive.
- [x] `downplay_defaults_apply()` overlays: `menu_driver = "downplay"` unconditionally; `input_menu_toggle_gamepad_combo = INPUT_COMBO_START_SELECT` only when current is `NONE`; path settings (`directory_menu_content`, `directory_system`, save/state dirs) routed under `Downplay/` only when current value is empty or matches the upstream RA platform default (so an explicit user override in `retroarch.cfg` is preserved). Called from `retroarch.c` after `config_load()` and before the CLI second pass. The other curated defaults (`content_history_size`, hidden-files, ~15–30 UI/input keys) are deferred to M7 polish — the prototype runs fine without them.
- [x] Asset shipping: `downplay/assets/downplay/InterTight-Bold.ttf` is bundled into the APK via `assets.srcDirs += ['../../../downplay/assets']` in `build.gradle`. RA's existing `bundle_assets` extract pipeline (already enabled on Android via `griffin.c`'s `#define HAVE_COMPRESSION 1`) copies it to `<dataDir>/assets/downplay/InterTight-Bold.ttf` on first launch and on every version bump. No new C code; we just feed the existing pipeline.
- [x] Verified end-to-end: clean uninstall + reinstall lands the user in the Downplay launcher with the bundled font and existing ROM tree, no `adb` interventions.

**Exit criterion:** wiping app data and reinstalling lands the user directly in a working Browse view with the storage layout already created. ✅

### M7 — Save state UX

Manual save states are still the headline here: slot picker, thumbnails, Save / Load rows in the in-game menu.  Auto-resume gets a smaller treatment than the equivalent feature in MinUI does.  Why: on Android, the OS keeps the app process resident across screen-off / lock with very little battery cost, so closing-and-reopening already lands the user back in the running game with no save-state involved.  MinUI needed auto-resume because its target devices fully power down to save battery; we don't have that constraint.

What we still want for the cold-restart case (force-stop, reboot, OOM kill): a `.state.auto` written on Quit and consumed on next launch of the same content.  RA already supports this; we just turn the flags on and surface the entry point.

**Manual save states (the bulk of this milestone):**
- In-game menu rows **Save State** / **Load State**, slot-based.  The active slot is `settings->ints.state_slot` and persists in retroarch.cfg already.
- Slot picker is a drill-in list (slots 0–9; configurable later).  Each row shows: slot number, timestamp from the file mtime, and (when `savestate_thumbnail_enable` is on) a small thumbnail RA writes alongside the state file.
- Save → write to current slot (`CMD_EVENT_SAVE_STATE`), no overwrite confirmation (slot picker is the confirmation).  Load → read from current slot (`CMD_EVENT_LOAD_STATE`).  Default to slot 0 for users who never touch the picker.
- "New" / "Delete" entries: out of scope for v1; the slot-overwrite-on-save model is enough.

**Cold-restart resume (the smaller piece):**
- Turn on `savestate_auto_save` and `savestate_auto_load` in `downplay_defaults_apply()` (overlay-with-default-guard, like the M6 paths).  RA writes `<savestate_dir>/<content>.state.auto` on core unload and reads it back the next time the same content launches.
- In-game menu **Quit** (today: `CMD_EVENT_UNLOAD_CORE`): with `auto_save` on, RA writes the auto-state as part of unload — no new code needed.  Verify the timing actually catches the state vs. firing too late.
- Launcher boot: if the most recent Recents entry has an `.state.auto` next to it, render a top-of-list **Resume <game>** row.  OK on that row launches the game; `auto_load` restores the state.  Reuse the M4 recents launch path.
- We do *not* auto-jump straight into the game on launch.  The Recents row is a one-tap path back; that's enough given how rarely the cold-restart case actually fires on Android.

**Slot retention:**
- Auto-state lives forever, overwritten only on next Quit.  Manual slots live forever, overwritten only by an explicit Save into the same slot.  No automatic cleanup; the user manages their own States/ folder (documented in the M6 README).

**Implementation strategy:**
- Defaults overlay (M6): `savestate_auto_save = true`, `savestate_auto_load = true`, `savestate_thumbnail_enable = true`, all guarded by the existing should-overlay predicate.
- New `DOWNPLAY_VIEW_SLOT_PICKER` view, similar shape to RECENTS — array of `{ slot_idx, mtime, thumb_path }` rebuilt on entry.  Thumbnails reuse RA's existing `gfx_thumbnail` machinery.
- "Resume" row at top of TOP view when the head of `g_defaults.content_history` has a `.state.auto`.  Detection is a `path_is_valid` against `<savestate_dir>/<basename>.state.auto`.

**Exit criterion:** save to slot 3, load from slot 3, see thumbnails in the picker.  Separately, force-stop the app mid-game, relaunch, pick the **Resume** row at the top of the launcher, land back exactly where you left off.

### M8 — In-game settings (MinUI-style)

In-game menu currently has Continue + Quit (M4.5) and the save-state rows added by M7.  M8 fills in the rest of what MinUI exposes: a curated, gamepad-navigable settings surface for per-core options plus the standard quick-menu actions.  Goal: never need to drop into XMB to tweak a game.

What the user sees, organized as in-game menu rows (in addition to the M7 state rows above):

- **Options** (per-core) — drill-in.  Iterate `runloop_state.core_options` (`runloop.h:220`); render each option's display name + current value label.  L/R cycles values; OK confirms.  Use `core_option_manager_get_val` / `core_option_manager_get_val_label` / set-by-index APIs in `core_option_manager.h`.  Only the options the core actually exposes — no RA-side categories.
- **Controls** — drill-in.  Per-core input remap.  Reuse RA's existing remap storage (`config/remaps/<core>/<content>.rmp` via `directory_input_remapping`) so anything we save here Just Works on subsequent loads.  Surface the standard libretro buttons only; advanced stuff (analog deadzone, turbo) deferred.
- **Disc** — drill-in, only when the core supports the disk-control interface.  Eject / Insert / pick disc index.  Driven by `CMD_EVENT_DISK_*`.
- **Reset** — `CMD_EVENT_RESET`, with a confirmation row.
- **Cheats** — drill-in.  Out of scope for v1; defer to M7.5 if there's demand.

What we deliberately do **not** expose: video drivers, audio drivers, latency, shaders, achievements, network, anything tied to RA infrastructure rather than the running game.  Those stay reachable via XMB only (see "XMB escape hatch" in Open questions).

System / launcher-level settings (a separate row from the top of Browse, not in-game): out of scope for this milestone.  Brightness / volume / sleep timer are explicitly *not* coming — Android handles those system-wide on our target devices.

**Implementation strategy:**

- Each row lives in the existing `DOWNPLAY_VIEW_INGAME` view; drill-ins reuse the same selection/scroll machinery as Browse.  No new view machinery.
- Per-core options drill-in is a new view (`DOWNPLAY_VIEW_INGAME_OPTIONS`) backed by an array of `{ idx, label, current_val_label }` cached on entry.  L/R cycles via `core_option_manager_set_val`; OK confirms; B exits and rebuilds the cache on re-entry.
- Save-state slot is a `settings->ints.state_slot`-style scalar persisted via the regular config-save path; no new storage.
- Remap save uses `task_push_save_remap_file` (already in `tasks/tasks_internal.h`) — no new patch points.

**Exit criterion:** during gameplay, the user can save and load state, change a per-core option (e.g., `mgba` color correction), remap a button, and eject/insert a disc — all from the Downplay in-game menu, without touching XMB.

### M9 — Device-aware configuration

Modern Android handhelds have wildly varying panel resolutions (480p to 1440p), aspect ratios (4:3, 3:2, 16:9, 16:10, 21:9), and refresh rates (60, 90, 120 Hz).  RA's compiled-in defaults aren't aware of any of this.  M9 picks sensible video / scaling / refresh defaults at install time from the actual panel — so a fresh boot looks right without the user opening a settings screen.

We're not doing per-core perf tuning (the target devices are fast enough that tuning is unnecessary) or theming (one fixed look).  We *are* doing the geometry work that makes a 1080×1920 portrait device and a 480×640 square device both look correct out of the box.

What gets derived from the panel:

- **Integer scaling** — `video_scale_integer = true` whenever the panel is high enough resolution to render the largest target system (≥3× SNES native ≈ 768 px tall) at integer scale without losing too much pixel real estate.  Falls back to non-integer with bilinear off for very low-res panels.
- **Aspect ratio handling** — `aspect_ratio_index = ASPECT_RATIO_CORE` (respect each core's native ratio) plus `video_aspect_ratio_auto = true`.  Single setting; per-core overrides not needed because the core ratio already encodes "GBA is 3:2, SNES is 4:3."
- **Refresh rate** — read the panel via the video driver (`video_driver_get_refresh_rate`) and write `video_refresh_rate` to match.  RA's vsync stays on; the swap-interval default is fine.  90/120 Hz panels: leave at 60 Hz for accuracy unless we add an explicit "smooth" toggle later.
- **Menu chrome scale** — Downplay's own font/padding/row sizing already derives from `video_height / REF_HEIGHT` (`menu/drivers/downplay.c:871`); the `user_scale_factor` knob stays at 1.0 unless we surface it later.  No code change needed; just confirm it looks right on the actual aspect ratios we ship to.
- **Orientation** — pin landscape on Android.  Portrait handhelds (pretty rare) get an explicit override later if needed.

Implementation strategy:

- Run *once per device profile change*, not every launch.  Cache `{ width, height, refresh_rate, density }` to `Downplay/.profile` after first detection; on subsequent launches, redetect and only re-overlay if the profile changed (e.g., user moved their save to a different device).
- Lives in a new `downplay_profile.{c,h}` module called from `downplay_defaults_apply()` after the path overlays.  Reads via existing video-driver getters; no new patch points.
- The overlay respects user overrides via the same `downplay_should_overlay` predicate used in M6 — if the user has explicitly set `video_refresh_rate`, we don't clobber it.

Non-goals: brightness, volume, sleep timer (Android overlays its own affordances for these and our handhelds inherit them); theming / paks; per-core performance tuning (`run-ahead` frames, latency tweaks).

**Exit criterion:** install on two devices with different panels (e.g., a 480×640 Anbernic and a 1080×1920 Retroid).  Without touching a setting, both render correctly: integer-scaled where the panel allows it, aspect ratio respected, refresh rate matched.

### M10 — Automatic content downloads + updates

M3 covers the headline case (download cores referenced by the user's folders).  M10 generalizes that idea: anything Downplay needs from the internet gets fetched silently and kept up to date, so the user never sees "go install X" or "your Y is out of date."  The user runs the app and it works; updates happen invisibly.

What needs to be auto-fetched / refreshed:

- **Cores** (extends M3) — pre-download every core referenced by a visible system folder, not just the ones launched yet.  Periodically check the buildbot for newer versions of installed cores (`task_push_update_installed_cores`, `tasks_internal.h:140`) and refresh in place.
- **Core info files** — the descriptors RA reads from `<info_dir>` to know each core's display name, supported extensions, etc.  These ship in a separate libretro-core-info repo and update independently of the cores themselves; refresh on the same cadence.
- **Joypad autoconfigs** — RA's `autoconfig_directory` of per-controller mapping profiles.  Critical on Android handhelds where a given device's built-in pad may or may not have a profile yet.  Auto-download on first boot via the existing online-updater task; refresh periodically.
- **Cheats database** — when the user (eventually) enables cheats from the in-game menu, the database should already be on disk.  Pre-fetch lazily on first cheats-row entry, not on boot.
- **Failed-download retry** — if the M3 boot pass couldn't reach the buildbot, retry on the next launch (or when network becomes available).  Today the splash times out and the user has to relaunch; M10 makes the retry automatic.

Policy decisions to nail down during implementation:

- **Frequency** — daily? weekly? on every boot? Probably "first launch + every N days, tracked via a timestamp in the cache."
- **Network awareness** — wifi-only by default; cellular requires a future toggle.  Android's `ConnectivityManager` exposes the metered-connection bit.
- **Failure visibility** — silent on success, silent on transient failure (we'll retry), surface only on persistent failure (e.g., a folder's core has been delisted from the buildbot — that's a real "this won't work" signal).
- **Progress UI** — boot-time pre-fetch reuses the M3 splash.  Post-boot updates are background-only (no UI) unless something the user is actively waiting on (e.g., a cheats lookup) is in flight.

**Implementation strategy:**

- Generalize the `downplay_cores` module into a small "online content sync" coordinator.  The single-flight install queue from M3 is a good shape; extend the queue to handle non-core download tasks (autoconfigs, info files) using the same async-task plumbing.
- New `downplay/downplay_sync.{c,h}` module — owns the cadence, the cache-timestamp file (`Downplay/.sync_cache`), and the dispatch.  Uses existing `task_push_*` APIs; no new upstream patch points expected.
- The "what cores does any folder reference" set already exists in the M3 collect-needed pass; reuse it for the broader pre-download.

**Exit criterion:** wipe the app, fly somewhere with no internet, plug in a controller Downplay has never seen.  After the first online launch (back home), the controller works the next time (autoconfig was fetched), every system folder's games launch (cores pre-fetched, not just the ones I'd previously played), and core/info updates have happened in the background without a single "downloading" notification.

### M11+ — Polish

System-folder visibility filtering (carryover from M3), cheats, upstream-rebase tooling, decisions about how/whether to expose XMB as a fallback.

## Maintenance discipline

- **Rebase, don't merge.** Periodically rebase the Downplay branch on `libretro/RetroArch` master. Squash Downplay commits when it makes the rebase cleaner.
- **One concept per patch.** When a new feature requires a new upstream patch point, that's its own commit, not bundled with implementation.
- **Marker comments.** Every line of upstream code we modify gets a `/* DOWNPLAY: ... */` marker. Greppable: `git grep DOWNPLAY` should enumerate the entire fork delta in upstream files.
- **Track upstream churn.** Before each rebase, skim `git log upstream/master -- menu/menu_driver.c menu/menu_driver.h frontend/frontend.c retroarch.c configuration.c Makefile.common` for changes near our patch points.

## Open questions

- How do we expose the "drop into RGUI/XMB" escape hatch on a gamepad-only device? (Hidden hotkey? A config-only toggle? No escape hatch at all?)
- Do we want a single "Downplay" Android build artifact, or do we keep the upstream RetroArch APK working in parallel for testing?
- Theming: do we copy MinUI's pak system, or stay simpler with a single fixed look until there's demand?
- **Core ident validation against the buildbot list is async.** Folders may flicker (briefly visible during boot before the buildbot list arrives, then hidden if the core isn't on it, or vice versa). Acceptable, or do we hold the menu until the list lands?
- **Buildbot unreachable.** First-launch with no network: do we show installed-only folders, or an explicit "no network" state? Subsequent launches can use the cached list from the previous boot.
- **PFD on Play Store.** If we ever want a Play Store build, we need a separate code path through `task_push_play_feature_delivery_core_install`. Defer.
- **What if the user types a typo in the parens?** E.g., `Super Nintendo (snes9xx)`. Folder is hidden. Discoverability of the convention matters — the bootstrap `README.txt` is the only signal.
- **Per-core config defaults**: shipped as a baked-in table, or as files in `Downplay/Configs/` that we write on first launch?

These are deferred until prototype data tells us what matters.

## Implementation notes (verified against the codebase)

Anchors collected during planning. Treat as starting points — line numbers drift with upstream rebases.

**Menu driver contract** (`menu/menu_driver.h:343`)
- Required entries: `init`, `free`, `frame`, `ident`. Everything else can be `NULL`; existing drivers leave many slots empty.
- Driver context registered in the `menu_ctx_drivers[]` table at `menu/menu_driver.c:331`.
- Selection: `menu_driver_find_driver()` at `menu/menu_driver.c:4620` matches the `ident` field against `settings->arrays.menu_driver`.
- Input: the driver does **not** see raw input. `menu_event()` at `menu/menu_driver.c:5117` translates input into `enum menu_action` values (UP, DOWN, OK, CANCEL, etc.) before any driver code runs.
- Custom action semantics: provide an `entry_action` callback to override the generic dispatcher (`generic_menu_entry_action()` at `menu/menu_driver.c:7544`).

**Content launching** (`tasks/task_content.c`)
- Fresh launch with explicit core: `task_push_load_content_with_new_core_from_menu()` at L2312. Reference call sequence: `menu/cbs/menu_cbs_ok.c:2095–2110`.
- Re-launch from history: `task_push_load_content_from_playlist_from_menu()` at L2056. Reference: `menu/cbs/menu_cbs_ok.c:2524`.
- Always set `menu_driver_set_last_start_content()` before launching for state consistency.
- After core exit, the runloop resumes the menu naturally (`runloop.c:6024,6062`); no reset call needed.

**Installed-core lookup** (`core_info.h`)
- `core_info_get_list(&core_list)` to obtain the loaded list. `core_info_find(core_path, &info)` (`core_info.h:193`) resolves a local core file path to its metadata; useful to confirm an entry on disk is a valid installed core.
- Note: `core_info_list_get_supported_cores(list, path, &infos, &num_infos)` (`core_info.h:158`) returns candidate cores for a *content path* by extension. Downplay does **not** use this for the primary flow — the core ident comes from the system folder name suffix. Kept here only as a fallback reference if a future feature needs extension-based resolution.

**Recents / history** (`playlist.h`)
- Source: `g_defaults.content_history` (a `playlist_t *`).
- Read: `playlist_get_size()` at L323, `playlist_get_index()` at L230. Each entry exposes `path`, `core_path`, `label`.
- Write (after launch): `playlist_push_runtime()` at `playlist.c:987` deduplicates and persists.

**Core updater (lazy-download flow)** (`core_updater_list.h`, `tasks/tasks_internal.h`, `tasks/task_core_updater.c`)
- Guarded by `HAVE_NETWORKING` (`tasks/tasks_internal.h:33`). Required for Downplay.
- Buildbot URL comes from `settings->paths.network_buildbot_url` (config-driven, not hardcoded).
- Cached list singleton: `core_updater_list_get_cached()` (`core_updater_list.c:43`).
- Fetch list (async task): `task_push_get_core_updater_list(list, mute, refresh_menu)` (`tasks/tasks_internal.h:129`, impl `task_core_updater.c:432`).
- Iterate list: `core_updater_list_size()` and `core_updater_list_get_index()` (`core_updater_list.h:112,122`). Each entry has `remote_filename`, `remote_core_path`, `local_core_path`, `display_name`.
- No reverse lookup by short ident — iterate and match `remote_filename` prefix (`snes9x_libretro.so.zip` starts with `snes9x_libretro`, derived from short ident `snes9x`).
- Check installed: `path_is_valid(entry->local_core_path)`, optionally `core_info_find()` (`core_info.h:193`).
- Download (async task): `task_push_core_updater_download(list, filename, crc, mute, auto_backup, hist_size, dir_libretro, dir_core_assets)` (`tasks/tasks_internal.h:134`, impl `task_core_updater.c:1002`).
- **Completion callback is hardcoded** (`cb_task_core_updater_download` at `task_core_updater.c:511`) — only reloads core info. Hooking "launch ROM after install" requires the additive patch in `tasks/tasks_internal.h` listed in the patch-points table.
- Android Play Feature Delivery branch at `task_core_updater.c:440–444` short-circuits to a different install path; disable PFD for the Downplay Android sideload build.

**Directory listing** (`libretro-common/include/lists/dir_list.h:64`, `libretro-common/include/retro_dirent.h:75`)
- High-level: `dir_list_new(dir, ext, include_dirs, include_hidden, include_compressed, recursive)` returns a `string_list *`. Sort with `dir_list_sort()`.
- Entry type is in `elems[i].attr.i` — check against `FILE_TYPE_DIRECTORY`.
- Streaming variant: `retro_opendir` / `retro_readdir` / `retro_dirent_get_name` / `retro_dirent_is_dir` / `retro_closedir`.
- Gotcha: `retro_readdir` returns `.` and `..` on Unix — filter them.

**Defaults overlay site** (`retroarch.c` ~L7497)
- Inside `retroarch_main_init()`, after `config_load()` returns. Upstream defaults + on-disk config have run; CLI override hasn't (second pass starts ~L7509). This is where `downplay_defaults_apply()` belongs.
- To make `"downplay"` the active menu driver from a fresh start, set `settings->arrays.menu_driver = "downplay"` here rather than editing `config_get_default_menu()` at `configuration.c:1456`.

**Android build & paths** (`pkg/android/phoenix-common/jni/Android.mk`, `frontend/drivers/platform_unix.c`)
- Android compiles via `griffin/griffin.c` (unity build) using objects listed in `Makefile.common`. New files need `HAVE_DOWNPLAY=1` in `Android.mk` (L89–162) to activate conditional includes.
- Storage permission tiers in `platform_unix.c:105–109`; default-dir population at L1810–1867.
- `g_defaults.dirs[DEFAULT_DIR_MENU_CONTENT]` is the right anchor for "where ROMs live" — Downplay should respect whatever Android sets it to rather than hardcoding paths.

**Asset loading** (`gfx/include/gfx/file_path_special.h:142–156`, `gfx/font_driver.c`)
- XMB pattern: `fill_pathname_application_special()` resolves `APPLICATION_SPECIAL_DIRECTORY_ASSETS_*` enum values to filesystem paths.
- Custom Downplay assets would require a new enum value (a contingent patch point). For prototype, reuse an existing font path (e.g., the RGUI font).

