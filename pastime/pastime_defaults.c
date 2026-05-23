/*  Pastime - a fork of RetroArch.
 *  Copyright (C) 2026 - Pastime contributors.
 *
 *  Pastime is free software: you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation, either version 3 of the License, or (at your option)
 *  any later version.
 *
 *  Pastime is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Pastime. If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <string.h>

#include <retro_miscellaneous.h>
#include <file/file_path.h>
#include <string/stdstring.h>

#include "pastime_defaults.h"

#include "../configuration.h"
#include "../defaults.h"
#include "../paths.h"
#include "../verbosity.h"
#include "../input/input_defines.h"
#include "../gfx/video_defines.h"

#ifdef ANDROID
#include <unistd.h>      /* access(), R_OK / W_OK */
#include <errno.h>       /* errno, for the EACCES probe diagnostics */
/* These globals live in platform_unix.c.  We declare extern locally
 * rather than including platform_unix.h because that header *defines*
 * (not declares) the arrays — including it from a separate TU would
 * duplicate the storage.  This is a pre-existing upstream RetroArch
 * quirk; the right fix would be marking them extern in the header and
 * keeping the definition in the .c file, but that's an upstream PR. */
extern char internal_storage_path[];
extern char internal_storage_app_path[];
/* Pastime-owned helpers defined alongside the JNI extra-reading site in
 * platform_unix.c.  Returns the count of removable mounts the Java side
 * discovered (StorageManager.getStorageVolumes()) and the i-th path. */
extern unsigned    pastime_removable_storage_count_get(void);
extern const char *pastime_removable_storage_path_get(unsigned i);
#endif

bool pastime_paths_get_root(char *out, size_t out_len)
{
#ifdef ANDROID
   /* Removable storage first.  We *only* adopt a card when it already
    * has a Pastime/ directory on it — never auto-bootstrap onto removable
    * media.  Card adoption is user-driven: stage the tree from a PC,
    * plug the card in, the launcher picks it up.  Internal storage stays
    * the default for fresh installs.  Adoptable-storage cards (encrypted
    * into the internal pool) don't appear here as separate entries — by
    * design, since they aren't portable across devices anyway. */
   {
      unsigned i, n = pastime_removable_storage_count_get();
      for (i = 0; i < n; i++)
      {
         char        candidate[PATH_MAX_LENGTH];
         const char *root = pastime_removable_storage_path_get(i);
         if (!root || !*root)
            continue;
         fill_pathname_join_special(candidate, root, "Pastime",
               sizeof(candidate));
         if (!path_is_directory(candidate))
            continue;
         /* MES grant doesn't guarantee FUSE/sdcardfs lets us read+write
          * — some OEM builds (Samsung primarily, but seen in the wild
          * on weird vendor forks) fence off `/storage/<UUID>/` to SAF
          * regardless of the granted permission.  Probe with access()
          * before adopting; on EACCES, fall through to internal so the
          * user has a working launcher rather than a tree they can
          * see-but-not-write.  access() is preferred over open() — no
          * file descriptor to leak, no TOCTOU footgun. */
         if (access(candidate, R_OK | W_OK) != 0)
         {
            RARCH_WARN("[Pastime] removable Pastime/ at %s exists but "
                  "is not read+write accessible (errno=%d %s); "
                  "falling back to internal storage.\n",
                  candidate, errno, strerror(errno));
            continue;
         }
         strlcpy(out, candidate, out_len);
         return true;
      }
   }

   if (*internal_storage_path)
   {
      fill_pathname_join_special(out, internal_storage_path, "Pastime", out_len);
      return true;
   }
   if (*internal_storage_app_path)
   {
      fill_pathname_join_special(out, internal_storage_app_path, "Pastime", out_len);
      return true;
   }
   return false;
#else
   {
      const char *home = getenv("HOME");
      if (!home || !*home)
         return false;
      fill_pathname_join_special(out, home, "Pastime", out_len);
      return true;
   }
#endif
}

/* True iff the current setting value is "either empty, or exactly the
 * upstream default RA computed for this platform" — i.e., the user has
 * not provided their own override.  Equality with the RA default lets
 * us re-apply our overlay on a fresh install where config_load() has
 * already populated the field with RA's defaults rather than leaving it
 * empty (Android does this for SRAM/SAVESTATE/SYSTEM via platform_unix.c). */
static bool pastime_should_overlay(const char *cur, const char *ra_default)
{
   if (!cur || !*cur)
      return true;
   if (ra_default && *ra_default && string_is_equal(cur, ra_default))
      return true;
   return false;
}

/* True iff `cur` looks like a path Pastime itself wrote on a prior boot
 * — i.e. it ends with "/Pastime/<leaf>".  We need this in addition to
 * pastime_should_overlay because the bare predicate is sticky: once
 * we've written `/storage/emulated/0/Pastime/Roms` to retroarch.cfg,
 * the next boot sees a non-empty, non-RA-default value there and skips
 * re-overlay — even when the user has now plugged in an SD card whose
 * `Pastime/Roms` should win.  Detecting "this is a Pastime tree path"
 * lets pastime_paths_get_root drive where the tree lives every boot,
 * not just on first run.  Explicit user overrides — paths that don't
 * end in `/Pastime/<leaf>` — are still respected. */
static bool pastime_owns_path(const char *cur, const char *leaf)
{
   /* "/Pastime/" + leaf, e.g. "/Pastime/Roms". */
   static const char prefix[] = "/Pastime/";
   const size_t      prefix_len = sizeof(prefix) - 1;
   size_t            cur_len, leaf_len, suffix_len;
   const char       *suffix;

   if (!cur || !*cur || !leaf || !*leaf)
      return false;

   cur_len    = strlen(cur);
   leaf_len   = strlen(leaf);
   suffix_len = prefix_len + leaf_len;
   if (cur_len < suffix_len)
      return false;

   suffix = cur + cur_len - suffix_len;
   /* The leading '/' in `prefix` IS the path-component boundary —
    * strncmp requires the trailing slice to begin with '/', so a string
    * like "/foo/NotPastime/Roms" doesn't match (its trailing 13 chars
    * are "tPastime/Roms", which fails the prefix compare).  No further
    * boundary check is needed. */
   if (strncmp(suffix, prefix, prefix_len) != 0)
      return false;
   return string_is_equal(suffix + prefix_len, leaf);
}

/* Combined predicate used at every path overlay site: write the
 * Pastime-rooted path when either (a) the user hasn't picked an
 * explicit override (pastime_should_overlay), or (b) the current value
 * is a path Pastime wrote previously and that we're now re-resolving
 * against a (possibly different) root. */
static bool pastime_should_route_path(const char *cur, const char *ra_default,
      const char *leaf)
{
   return    pastime_should_overlay(cur, ra_default)
          || pastime_owns_path(cur, leaf);
}

void pastime_defaults_apply(void)
{
   char        root[PATH_MAX_LENGTH];
   char        sub[PATH_MAX_LENGTH];
   settings_t *settings = config_get_ptr();

   if (!settings)
      return;

   /* Menu driver: unconditional override.  This is the fork — the whole
    * point is the Pastime UI.  XMB/Ozone are still compiled in for
    * debugging via a different binary or config, but the default ships
    * Pastime. */
   strlcpy(settings->arrays.menu_driver, "pastime",
         sizeof(settings->arrays.menu_driver));

#ifdef ANDROID
   /* Force Vulkan on Android.  Upstream defaults to legacy GLES2 (the
    * "gl" driver) which is glsl-only — slang shaders silently fail to
    * load, so the Frontend submenu's Effect/Crisp rows have nothing
    * to drive.  Snapdragon 800-series and Mali-Gxx have shipped solid
    * Vulkan for years; falling back to gl/glcore on devices without
    * Vulkan is the user's call via an explicit override. */
   /* "gl" is the upstream Android default (configuration.c:455 picks
    * VIDEO_GL when HAVE_OPENGLES is set, before HAVE_VULKAN gets a
    * chance) — equality with that string lets us re-overlay on a
    * fresh install without trampling an explicit user choice. */
   if (pastime_should_overlay(settings->arrays.video_driver, "gl"))
      strlcpy(settings->arrays.video_driver, "vulkan",
            sizeof(settings->arrays.video_driver));
#endif

   /* Gamepad menu combo: pick START+SELECT only when the user hasn't
    * chosen a combo (NONE).  Any explicit choice — L3+R3, hold START,
    * etc. — wins. */
   if (settings->uints.input_menu_toggle_gamepad_combo == INPUT_COMBO_NONE)
      settings->uints.input_menu_toggle_gamepad_combo = INPUT_COMBO_START_SELECT;

   /* Save-state UX (M7): autosave on quit, autoload on launch, and
    * always capture a screenshot.  These power Resume in the launcher
    * and the thumbnails in the in-game load picker.  Applied
    * unconditionally — the boolean upstream defaults are all false, so
    * we can't distinguish "user turned it off" from "never set", and
    * the Pastime UX depends on these being on. */
   settings->bools.savestate_auto_save        = true;
   settings->bools.savestate_auto_load        = true;
   settings->bools.savestate_thumbnail_enable = true;

   /* Box-art on demand.  RA defaults this to false ("download manually
    * via the in-menu task"), but Pastime's launcher fires the load on
    * row hover — without on-demand, the resolver only checks local
    * disk, every fresh ROM 404s into the placeholder rect, and the
    * user has no visible affordance to fix it.  Applied unconditionally
    * for the same reason as savestate_thumbnail_enable above: the
    * Pastime UX depends on it being on, and we can't tell "user
    * disabled" from "never set". */
   settings->bools.network_on_demand_thumbnails = true;

   /* Disable the builtin image viewer.  RA's main init (retroarch.c
    * ~8200) rewrites any ".png"/.jpg" content load to CORE_TYPE_IMAGEVIEWER
    * regardless of the libretro core path the menu picked — which
    * hijacks PICO-8 cartridges (".p8.png" — PNG-encoded carts) when
    * launched via fake-08.  We never want the builtin viewer in the
    * launcher path; toggle it off unconditionally. */
   settings->bools.multimedia_builtin_imageviewer_enable = false;

   /* Log to file by default.  On Android, post-init RARCH_LOG goes to
    * stderr which is invisible — without this, every diagnostic we
    * emit (and every upstream RA warning we'd want to see) just
    * vanishes.  Cheap; the file gets rotated by RA itself. */
   settings->bools.log_to_file = true;

   /* Frontend video default: Screen Scaling = Aspect (core PAR,
    * fractional fill).  Integer scaling here would leave thick borders
    * on handhelds — the Frontend submenu's Native row is the integer-
    * locked alternative.  Applied unconditionally since RA's aspect
    * default elsewhere can be ASPECT_RATIO_CONFIG / 16:9 / 4:3
    * depending on platform.  video_smooth is intentionally not pinned
    * — RA's default is nearest on every Pastime target platform, and
    * the Sharpness row was dropped (Vulkan/GLCore have no runtime
    * filter toggle), so any user-set override should be respected. */
   settings->uints.video_aspect_ratio_idx     = ASPECT_RATIO_CORE;
   settings->bools.video_scale_integer        = false;

   /* In-game settings persistence model (M8).  Two pieces:
    *
    * 1. config_save_on_exit = false: prevents settings_t from being
    *    flushed to retroarch.cfg at process quit.  All in-game
    *    settings changes are session-only by default; persistence
    *    requires the user to explicitly hit Save for console / Save
    *    for game in the in-game menu, which writes a per-core or
    *    per-game override file (configuration.c stacks overrides on
    *    top of the base cfg automatically on next launch).
    *    Belt-and-suspenders against the launcher path — when content
    *    isn't loaded there's no override active, so the override
    *    block in config_save_file (configuration.c:5844) doesn't
    *    apply, and any settings changed from a launcher menu would
    *    leak globally without this.
    *
    *    Note: this overlay is unconditional, not gated by a
    *    "should_overlay" check.  A user who manually sets
    *    config_save_on_exit = true in their retroarch.cfg will see
    *    it silently reverted on every boot — that's deliberate, not
    *    a bug.  The session-only persistence model depends on this
    *    flag being off, and toggling it would silently break the
    *    Frontend submenu's commit semantics.
    *
    * 2. auto_shaders_enable = true: gates RA's per-core / per-game
    *    .slangp auto-load on core+content launch.  The Save for
    *    console / game actions write these files via
    *    menu_shader_manager_save_auto_preset; without auto-load the
    *    saved preset wouldn't take effect on the next session. */
   settings->bools.config_save_on_exit        = false;
   settings->bools.auto_shaders_enable        = true;

   /* Hide the noisy upstream OSD bits that don't fit the launcher's quiet
    * aesthetic.  Disables the load-content splash, the modern widget
    * notifications, and *all* legacy OSD text — including FPS, shader
    * parameter overlays, error messages, and core-emitted RETRO_MESSAGE
    * strings.  Coarse on purpose; M8 will expose a user toggle. */
   settings->bools.menu_show_load_content_animation = false;
   settings->bools.menu_enable_widgets              = false;
   settings->bools.video_font_enable                = false;

   /* Long-list nav: with menu_scroll_fast off (RA default) the cursor
    * advances at most one row per repeat tick, which feels constant-
    * speed on a 200-ROM list even though the framework is already
    * auto-accelerating the repeat rate.  Flipping this on raises the
    * acceleration cap (menu_driver.c:5382) so the per-tick step
    * scales to multiple rows under a sustained hold, giving the
    * LessUI "chew through a long list quickly" feel. */
   settings->bools.menu_scroll_fast = true;

   if (!pastime_paths_get_root(root, sizeof(root)))
   {
      RARCH_WARN("[Pastime] could not resolve Pastime/ root; "
            "leaving paths at upstream defaults\n");
      return;
   }

   /* Path overlays.  pastime_should_route_path covers two cases: a
    * fresh install where the value is empty/RA-default, and a re-boot
    * where the value is a path *we* wrote previously — letting an SD
    * card with Pastime/ on it win over an internal-storage tree we
    * baked into the cfg on a prior boot. */
   if (pastime_should_route_path(settings->paths.directory_menu_content,
            g_defaults.dirs[DEFAULT_DIR_MENU_CONTENT], "Roms"))
   {
      fill_pathname_join_special(sub, root, "Roms", sizeof(sub));
      strlcpy(settings->paths.directory_menu_content, sub,
            sizeof(settings->paths.directory_menu_content));
   }
   if (pastime_should_route_path(settings->paths.directory_system,
            g_defaults.dirs[DEFAULT_DIR_SYSTEM], "Bios"))
   {
      fill_pathname_join_special(sub, root, "Bios", sizeof(sub));
      strlcpy(settings->paths.directory_system, sub,
            sizeof(settings->paths.directory_system));
   }
   /* SAVEFILE / SAVESTATE live in the global dir_* slots, not in
    * settings->paths.  configuration.c maps the cfg keys
    * "savefile_directory" / "savestate_directory" through dir_get_ptr. */
   {
      char *cur = dir_get_ptr(RARCH_DIR_SAVEFILE);
      if (pastime_should_route_path(cur,
               g_defaults.dirs[DEFAULT_DIR_SRAM], "Saves"))
      {
         fill_pathname_join_special(sub, root, "Saves", sizeof(sub));
         dir_set(RARCH_DIR_SAVEFILE, sub);
      }
   }
   {
      char *cur = dir_get_ptr(RARCH_DIR_SAVESTATE);
      if (pastime_should_route_path(cur,
               g_defaults.dirs[DEFAULT_DIR_SAVESTATE], "States"))
      {
         fill_pathname_join_special(sub, root, "States", sizeof(sub));
         dir_set(RARCH_DIR_SAVESTATE, sub);
      }
   }

   /* Box-art / thumbnails landing pad.  Lives under the user-facing
    * Pastime/ tree because (a) PNGs are user-visible data they may want
    * to rsync with their library, and (b) the libretro-thumbnails
    * directory layout (<system>/Named_Boxarts/<label>.png) is portable
    * across any RA-compatible tooling.  Stock RA defaults to
    * <config>/thumbnails/ — re-route either when that's still the
    * value or when we previously wrote a Pastime/Thumbnails path. */
   if (pastime_should_route_path(settings->paths.directory_thumbnails,
            g_defaults.dirs[DEFAULT_DIR_THUMBNAILS], "Thumbnails"))
   {
      fill_pathname_join_special(sub, root, "Thumbnails", sizeof(sub));
      strlcpy(settings->paths.directory_thumbnails, sub,
            sizeof(settings->paths.directory_thumbnails));
   }

   /* Boot log only.  defaults_apply now runs on every cfg reload
    * (game exit, CMD_EVENT_RELOAD_CONFIG, override stack reload),
    * which would spam this line on every short session. */
   {
      static bool logged = false;
      if (!logged)
      {
         RARCH_LOG("[Pastime] defaults applied; root=%s\n", root);
         logged = true;
      }
   }
}
