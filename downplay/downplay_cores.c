/*  Downplay - a fork of RetroArch.
 *  Copyright (C) 2026 - Downplay contributors.
 *
 *  Downplay is free software: you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation, either version 3 of the License, or (at your option)
 *  any later version.
 *
 *  Downplay is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Downplay. If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <string.h>

#include <features/features_cpu.h>

#include "downplay_cores.h"

#include "../core_info.h"
#include "../core_updater_list.h"
#include "../configuration.h"
#include "../tasks/tasks_internal.h"
#include "../verbosity.h"

/* If the buildbot list fetch hasn't returned anything within this many
 * microseconds we give up and dismiss the splash.  The legitimate-empty
 * (server hiccup) and silent-fetch-failure cases both look like "list
 * size 0" indefinitely without this timeout. */
#define DOWNPLAY_CORES_LIST_TIMEOUT_USEC ((retro_time_t)15 * 1000 * 1000)

/* Single-flight install queue.  Sequential by design (see PLAN.md M3):
 * one core at a time, simpler progress UI, friendlier to the buildbot. */
typedef struct
{
   enum downplay_cores_state state;
   char                    **idents;        /* heap-owned, deduped, missing-only */
   size_t                    count;
   size_t                    cursor;        /* index of currently-installing item */
   retro_time_t              await_started; /* usec; 0 outside AWAITING_LIST */
   retro_task_t             *list_task;     /* the buildbot list-fetch task; NULL after finish */
   bool                      cancelled;     /* set true by cancel; pump short-circuits */
} downplay_cores_t;

static downplay_cores_t g_state;

/* ---------- helpers ---------- */

static void downplay_cores_clear_queue(void)
{
   size_t i;
   for (i = 0; i < g_state.count; i++)
      free(g_state.idents[i]);
   free(g_state.idents);
   g_state.idents        = NULL;
   g_state.count         = 0;
   g_state.cursor        = 0;
   g_state.await_started = 0;
   g_state.list_task     = NULL;
   g_state.cancelled     = false;
}

bool downplay_cores_is_installed(const char *core_ident)
{
   char         lookup[256];
   core_info_t *info = NULL;

   if (!core_ident || !*core_ident)
      return false;
   snprintf(lookup, sizeof(lookup), "%s_libretro", core_ident);
   return core_info_find(lookup, &info) && info && info->path;
}

/* Match a core_updater_list entry by ident.  remote_filename looks like
 * "<ident>_libretro.<ext>.zip" — anchor on "<ident>_libretro" so a
 * substring match (e.g. "snes" hitting "snes9x_libretro") can't fool us. */
static const core_updater_list_entry_t *downplay_cores_find_entry(
      core_updater_list_t *list, const char *ident)
{
   size_t       i;
   size_t       ident_len;
   const char  *suffix     = "_libretro";
   size_t       suffix_len = strlen(suffix);
   size_t       n;

   if (!list || !ident)
      return NULL;
   ident_len = strlen(ident);
   n         = core_updater_list_size(list);
   for (i = 0; i < n; i++)
   {
      const core_updater_list_entry_t *cur = NULL;
      const char                      *fn;
      if (!core_updater_list_get_index(list, i, &cur) || !cur)
         continue;
      fn = cur->remote_filename;
      if (!fn)
         continue;
      if (strncmp(fn, ident, ident_len) != 0)
         continue;
      if (strncmp(fn + ident_len, suffix, suffix_len) != 0)
         continue;
      /* Char after "_libretro" must be a separator — '.' (extension) or
       * '_' (platform suffix like "_libretro_android.so").  This rules
       * out e.g. "_libretroplus" matching "_libretro". */
      {
         char c = fn[ident_len + suffix_len];
         if (c != '.' && c != '_' && c != '\0')
            continue;
      }
      return cur;
   }
   return NULL;
}

/* Forward decl: completion callback for a single download. */
static void downplay_cores_install_done_cb(retro_task_t *task,
      void *task_data, void *user_data, const char *err);

/* Walk the queue from cursor forward, skipping idents that aren't on the
 * buildbot, and queue the first one that is.  Lands in DONE when the
 * queue is exhausted. */
static void downplay_cores_queue_next(void)
{
   core_updater_list_t *list     = core_updater_list_get_cached();
   settings_t          *settings = config_get_ptr();

   if (!list || !settings)
   {
      g_state.state = DOWNPLAY_CORES_DONE;
      return;
   }

   while (g_state.cursor < g_state.count)
   {
      const char                      *ident = g_state.idents[g_state.cursor];
      const core_updater_list_entry_t *entry =
            downplay_cores_find_entry(list, ident);
      if (!entry)
      {
         /* Not on the buildbot — silently skip per PLAN ("no broken
          * folder UX").  System-folder visibility filtering will hide
          * the corresponding row separately. */
         RARCH_LOG("[Downplay] core not on buildbot: %s\n", ident);
         g_state.cursor++;
         continue;
      }

      /* Register completion before queuing the task — the underlying
       * task only stores task->callback once, so we need our hook armed
       * before the task could possibly finish. */
      task_core_updater_set_download_callback(
            downplay_cores_install_done_cb, NULL);

      RARCH_LOG("[Downplay] downloading core: %s\n", entry->remote_filename);
      /* CRC arg is the LOCAL file's CRC (not the remote's).  Passing
       * the remote CRC short-circuits the task as "already installed"
       * even though no file is on disk; pass 0 to let it compute the
       * local CRC (which is 0 for a missing file → forces download). */
      if (!task_push_core_updater_download(list,
            entry->remote_filename, 0 /* local crc */,
            true /* mute */, false /* auto_backup */, 0,
            settings->paths.directory_libretro,
            settings->paths.directory_core_assets))
      {
         /* Push refused (locked core, dup task in flight, missing path).
          * Skip and continue — our cb won't fire if no task was queued. */
         RARCH_WARN("[Downplay] core download refused: %s\n",
               entry->remote_filename);
         task_core_updater_set_download_callback(NULL, NULL);
         g_state.cursor++;
         continue;
      }
      g_state.state = DOWNPLAY_CORES_INSTALLING;
      return;
   }

   g_state.state = DOWNPLAY_CORES_DONE;
}

static void downplay_cores_install_done_cb(retro_task_t *task,
      void *task_data, void *user_data, const char *err)
{
   (void)task;
   (void)task_data;
   (void)user_data;

   if (err && *err)
      RARCH_WARN("[Downplay] core install failed at idx %u: %s\n",
            (unsigned)g_state.cursor, err);

   /* Whether success or fail, advance.  Failed installs leave the user
    * to retry (or hit the lazy fallback at launch time). */
   g_state.cursor++;

   if (g_state.cancelled)
   {
      g_state.state = DOWNPLAY_CORES_DONE;
      return;
   }

   downplay_cores_queue_next();
}

/* ---------- public API ---------- */

void downplay_cores_begin_boot_setup(const char *const *idents, size_t count)
{
   size_t i;
   size_t needed = 0;

   /* Reset any prior pass.  If a download is currently in flight we
    * can't interrupt it (the task has no cancel hook), but our hook
    * slot in the upstream task module is single-shot — when the in-
    * flight download finishes it will reach for the slot we're about
    * to overwrite below in queue_next().  Belt and suspenders. */
   downplay_cores_clear_queue();

   if (!idents || count == 0)
   {
      g_state.state = DOWNPLAY_CORES_DONE;
      return;
   }

   /* Filter to missing + deduped. */
   g_state.idents = (char**)calloc(count, sizeof(char*));
   if (!g_state.idents)
   {
      g_state.state = DOWNPLAY_CORES_DONE;
      return;
   }
   for (i = 0; i < count; i++)
   {
      const char *ident = idents[i];
      size_t      j;
      bool        seen  = false;

      if (!ident || !*ident)
         continue;
      if (downplay_cores_is_installed(ident))
         continue;
      for (j = 0; j < needed; j++)
      {
         if (strcmp(g_state.idents[j], ident) == 0)
         {
            seen = true;
            break;
         }
      }
      if (seen)
         continue;
      if (!(g_state.idents[needed] = strdup(ident)))
         continue;
      needed++;
   }
   g_state.count = needed;

   if (needed == 0)
   {
      free(g_state.idents);
      g_state.idents = NULL;
      g_state.state  = DOWNPLAY_CORES_DONE;
      return;
   }

   /* Ensure the cached list singleton exists, then kick the fetch.
    * The mute=true flag suppresses the "downloading list…" notification
    * we'd otherwise see during boot. */
   if (!core_updater_list_get_cached())
      core_updater_list_init_cached();
   {
      core_updater_list_t *list = core_updater_list_get_cached();
      g_state.list_task = (retro_task_t*)task_push_get_core_updater_list(
            list, true /* mute */, false /* refresh_menu */);
   }
   g_state.await_started = cpu_features_get_time_usec();
   g_state.state         = DOWNPLAY_CORES_AWAITING_LIST;
}

void downplay_cores_pump(void)
{
   core_updater_list_t *list;
   bool                 list_ready = false;

   if (g_state.state != DOWNPLAY_CORES_AWAITING_LIST)
      return;

   /* The buildbot list parser populates the cached list incrementally
    * from a worker thread (parse_network_data runs synchronously, but
    * task processing can happen off-main on Android).  Polling
    * `core_updater_list_size > 0` would race — find_entry then misses
    * later entries and reports "not on buildbot" for cores that
    * actually exist.  Wait until the list task is FINISHED, which is
    * after the parser has appended every line. */
   if (g_state.list_task)
   {
      uint8_t flg = task_get_flags(g_state.list_task);
      if ((flg & RETRO_TASK_FLG_FINISHED) != 0)
      {
         g_state.list_task = NULL;
         list_ready        = true;
      }
   }
   else
      list_ready = true;

   list = core_updater_list_get_cached();
   if (list_ready && list && core_updater_list_size(list) > 0)
   {
      /* List arrived — start downloading.  queue_next handles the empty
       * case (lands in DONE). */
      downplay_cores_queue_next();
      return;
   }

   /* Empty / unfetched list.  Distinguishing "still in flight" from
    * "fetch failed" without another patch point isn't possible, so
    * fall back to a wall-clock timeout: dismiss the splash and let the
    * lazy-launch fallback handle individual missing cores later. */
   if (cpu_features_get_time_usec() - g_state.await_started
         > DOWNPLAY_CORES_LIST_TIMEOUT_USEC)
   {
      RARCH_WARN("[Downplay] core updater list timed out; giving up\n");
      g_state.state = DOWNPLAY_CORES_DONE;
   }
}

enum downplay_cores_state downplay_cores_get_state(void)
{
   return g_state.state;
}

const char *downplay_cores_get_progress(size_t *out_done, size_t *out_total)
{
   if (out_done)
      *out_done = g_state.cursor;
   if (out_total)
      *out_total = g_state.count;
   if (g_state.state != DOWNPLAY_CORES_INSTALLING
         || g_state.cursor >= g_state.count)
      return NULL;
   return g_state.idents[g_state.cursor];
}

void downplay_cores_cancel(void)
{
   if (g_state.state == DOWNPLAY_CORES_INACTIVE
         || g_state.state == DOWNPLAY_CORES_DONE)
      return;
   if (g_state.state == DOWNPLAY_CORES_AWAITING_LIST)
   {
      g_state.state = DOWNPLAY_CORES_DONE;
      return;
   }
   /* INSTALLING: let the in-flight task finish; the callback will see
    * cancelled=true and short-circuit instead of queuing the next. */
   g_state.cancelled = true;
}
