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

#include <features/features_cpu.h>
#include <file/archive_file.h>
#include <file/file_path.h>
#include <streams/file_stream.h>

#include "pastime_cores.h"
#include "pastime_cores_extras.h"

#include "../command.h"
#include "../core_info.h"
#include "../core_updater_list.h"
#include "../configuration.h"
#include "../tasks/tasks_internal.h"
#include "../verbosity.h"

/* If the buildbot list fetch hasn't returned anything within this many
 * microseconds we give up and dismiss the splash.  The legitimate-empty
 * (server hiccup) and silent-fetch-failure cases both look like "list
 * size 0" indefinitely without this timeout. */
#define PASTIME_CORES_LIST_TIMEOUT_USEC ((retro_time_t)15 * 1000 * 1000)

/* Single-flight install queue.  Sequential by design (see PLAN.md M3):
 * one core at a time, simpler progress UI, friendlier to the buildbot. */
typedef struct
{
   enum pastime_cores_state state;
   char                    **idents;        /* heap-owned, deduped, missing-only */
   size_t                    count;
   size_t                    cursor;        /* index of currently-installing item */
   retro_time_t              await_started; /* usec; 0 outside AWAITING_LIST */
   retro_task_t             *list_task;     /* the buildbot list-fetch task; NULL after finish */
   bool                      cancelled;     /* set true by cancel; pump short-circuits */
} pastime_cores_t;

static pastime_cores_t g_state;

/* ---------- helpers ---------- */

static void pastime_cores_clear_queue(void)
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

bool pastime_cores_is_installed(const char *core_ident)
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
static const core_updater_list_entry_t *pastime_cores_find_entry(
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
static void pastime_cores_install_done_cb(retro_task_t *task,
      void *task_data, void *user_data, const char *err);

/* ---------- extras (non-buildbot) install path ---------- */

/* Build "<archive>#<inner>" — the path-with-fragment form that
 * file_archive_compressed_read uses to address a single zip entry. */
static void pastime_cores_extras_join_inner(char *out, size_t out_len,
      const char *archive, const char *inner)
{
   size_t n = strlcpy(out, archive, out_len);
   if (n + 1 < out_len)
   {
      out[n] = '#';
      strlcpy(out + n + 1, inner, out_len - n - 1);
   }
}

/* Pull one entry out of an on-disk zip and write it to dest_path under
 * its canonical name.  Returns true on success. */
static bool pastime_cores_extras_extract_one(const char *archive_path,
      const char *inner_path, const char *dest_path)
{
   char    addr[PATH_MAX_LENGTH];
   void   *buf = NULL;
   int64_t len = 0;
   bool    ok;

   pastime_cores_extras_join_inner(addr, sizeof(addr),
         archive_path, inner_path);
   if (!file_archive_compressed_read(addr, &buf, NULL, &len) || !buf
         || len <= 0)
   {
      RARCH_WARN("[Pastime] extras: extract failed for %s\n", inner_path);
      if (buf)
         free(buf);
      return false;
   }

   /* Ensure parent dir exists.  path_basedir mutates its buffer. */
   {
      char parent[PATH_MAX_LENGTH];
      strlcpy(parent, dest_path, sizeof(parent));
      path_basedir_wrapper(parent);
      if (*parent && !path_is_directory(parent) && !path_mkdir(parent))
      {
         RARCH_WARN("[Pastime] extras: mkdir failed: %s\n", parent);
         free(buf);
         return false;
      }
   }

   ok = filestream_write_file(dest_path, buf, (int64_t)len);
   free(buf);
   if (!ok)
      RARCH_WARN("[Pastime] extras: write failed: %s\n", dest_path);
   return ok;
}

/* http callback for an extras zip download.  Stages the zip to disk,
 * extracts the .so + .info under their canonical names, refreshes core
 * info, then advances the install queue.  user_data is the static
 * extras-table entry pointer (no lifetime concerns).
 *
 * On any failure we still advance — same policy as the buildbot path
 * (lazy launch fallback retries individual missing cores later). */
static void pastime_cores_extras_http_cb(retro_task_t *task,
      void *task_data, void *user_data, const char *err)
{
   const pastime_cores_extra_t *extra =
         (const pastime_cores_extra_t*)user_data;
   http_transfer_data_t        *data  = (http_transfer_data_t*)task_data;
   settings_t                  *settings = config_get_ptr();
   char                         tmp_zip[PATH_MAX_LENGTH];
   char                         dest_so[PATH_MAX_LENGTH];
   char                         dest_info[PATH_MAX_LENGTH];
   char                         tmp_basename[256];
   char                         so_basename[256];
   char                         info_basename[256];
   bool                         have_zip = false;
   bool                         install_ok = false;

   (void)task;

   if (!extra || !settings)
      goto advance;

   if (g_state.cancelled)
      goto advance;

   if (err && *err)
   {
      RARCH_WARN("[Pastime] extras http failed for %s: %s\n",
            extra->ident, err);
      goto advance;
   }
   if (!data || !data->data || data->len <= 0)
   {
      RARCH_WARN("[Pastime] extras http empty for %s\n", extra->ident);
      goto advance;
   }
   if (!settings->paths.directory_libretro[0]
         || !settings->paths.path_libretro_info[0])
   {
      RARCH_WARN("[Pastime] extras: core dirs unset; skipping %s\n",
            extra->ident);
      goto advance;
   }

   /* Stage the zip in directory_libretro under a per-ident temp name.
    * Same dir as the eventual .so, so any leftover after a crash sits
    * next to the install and is easy to spot. */
   snprintf(tmp_basename, sizeof(tmp_basename),
         "%s_libretro.tmp.zip", extra->ident);
   fill_pathname_join_special(tmp_zip,
         settings->paths.directory_libretro, tmp_basename, sizeof(tmp_zip));

   if (!path_is_directory(settings->paths.directory_libretro)
         && !path_mkdir(settings->paths.directory_libretro))
   {
      RARCH_WARN("[Pastime] extras: libretro dir missing: %s\n",
            settings->paths.directory_libretro);
      goto advance;
   }
   if (!filestream_write_file(tmp_zip, data->data, data->len))
   {
      RARCH_WARN("[Pastime] extras: stage write failed: %s\n", tmp_zip);
      goto advance;
   }
   have_zip = true;

   /* Canonical install names — see pastime_cores_extras.h.  The .so
    * filename must yield core_info ID "<ident>_libretro" via the scanner
    * rule (last underscore segment != "_libretro" → truncated).
    * "<ident>_libretro_android.so" satisfies that and mirrors the
    * buildbot's Android filenames. */
   snprintf(so_basename, sizeof(so_basename),
         "%s_libretro_android.so", extra->ident);
   snprintf(info_basename, sizeof(info_basename),
         "%s_libretro.info", extra->ident);
   fill_pathname_join_special(dest_so,
         settings->paths.directory_libretro, so_basename, sizeof(dest_so));
   fill_pathname_join_special(dest_info,
         settings->paths.path_libretro_info, info_basename,
         sizeof(dest_info));

   if (!pastime_cores_extras_extract_one(tmp_zip,
         extra->zip_so_path, dest_so))
      goto advance;
   if (extra->zip_info_path && *extra->zip_info_path
         && !pastime_cores_extras_extract_one(tmp_zip,
               extra->zip_info_path, dest_info))
   {
      /* .so already on disk — leave it; without the .info the scanner
       * won't match it to a core, but a future run that ships the .info
       * will recover.  Fall through to refresh anyway. */
      RARCH_WARN("[Pastime] extras: .info missing for %s, core may be hidden\n",
            extra->ident);
   }

   install_ok = true;
   RARCH_LOG("[Pastime] extras installed: %s\n", extra->ident);

advance:
   if (have_zip && filestream_exists(tmp_zip))
      filestream_delete(tmp_zip);

   if (install_ok)
      command_event(CMD_EVENT_CORE_INFO_INIT, NULL);

   /* Reuse the buildbot completion path so progress UI + cancel handling
    * stay single-sourced.  err is propagated for logging parity. */
   pastime_cores_install_done_cb(NULL, NULL, NULL, err);
}

/* Queue a download for an extras entry.  Returns false if the http push
 * was refused (caller should skip the ident, same as buildbot). */
static bool pastime_cores_extras_queue(const pastime_cores_extra_t *extra)
{
   if (!extra || !extra->zip_url || !*extra->zip_url)
      return false;
   RARCH_LOG("[Pastime] downloading extras core: %s (%s)\n",
         extra->ident, extra->zip_url);
   if (!task_push_http_transfer(extra->zip_url, true /* mute */,
         NULL /* type */,
         pastime_cores_extras_http_cb, (void*)extra))
   {
      RARCH_WARN("[Pastime] extras http push refused: %s\n", extra->ident);
      return false;
   }
   return true;
}

/* Walk the queue from cursor forward, skipping idents that aren't on the
 * buildbot, and queue the first one that is.  Lands in DONE when the
 * queue is exhausted. */
static void pastime_cores_queue_next(void)
{
   core_updater_list_t *list     = core_updater_list_get_cached();
   settings_t          *settings = config_get_ptr();

   if (!list || !settings)
   {
      g_state.state = PASTIME_CORES_DONE;
      return;
   }

   while (g_state.cursor < g_state.count)
   {
      const char                      *ident = g_state.idents[g_state.cursor];
      const core_updater_list_entry_t *entry =
            pastime_cores_find_entry(list, ident);
      if (!entry)
      {
         /* Not on the buildbot — try the curated extras table for
          * cores we ship via GitHub releases (e.g. fake-08 for PICO-8).
          * Miss in both = silently skip per PLAN ("no broken folder
          * UX"); system-folder visibility filtering hides the row. */
         const pastime_cores_extra_t *extra =
               pastime_cores_extras_lookup(ident);
         if (extra && pastime_cores_extras_queue(extra))
         {
            g_state.state = PASTIME_CORES_INSTALLING;
            return;
         }
         if (extra)
            RARCH_WARN("[Pastime] extras queue refused: %s\n", ident);
         else
            RARCH_LOG("[Pastime] core not on buildbot: %s\n", ident);
         g_state.cursor++;
         continue;
      }

      /* Register completion before queuing the task — the underlying
       * task only stores task->callback once, so we need our hook armed
       * before the task could possibly finish. */
      task_core_updater_set_download_callback(
            pastime_cores_install_done_cb, NULL);

      RARCH_LOG("[Pastime] downloading core: %s\n", entry->remote_filename);
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
         RARCH_WARN("[Pastime] core download refused: %s\n",
               entry->remote_filename);
         task_core_updater_set_download_callback(NULL, NULL);
         g_state.cursor++;
         continue;
      }
      g_state.state = PASTIME_CORES_INSTALLING;
      return;
   }

   g_state.state = PASTIME_CORES_DONE;
}

static void pastime_cores_install_done_cb(retro_task_t *task,
      void *task_data, void *user_data, const char *err)
{
   (void)task;
   (void)task_data;
   (void)user_data;

   if (err && *err)
      RARCH_WARN("[Pastime] core install failed at idx %u: %s\n",
            (unsigned)g_state.cursor, err);

   /* Whether success or fail, advance.  Failed installs leave the user
    * to retry (or hit the lazy fallback at launch time). */
   g_state.cursor++;

   if (g_state.cancelled)
   {
      g_state.state = PASTIME_CORES_DONE;
      return;
   }

   pastime_cores_queue_next();
}

/* ---------- public API ---------- */

void pastime_cores_begin_boot_setup(const char *const *idents, size_t count)
{
   size_t i;
   size_t needed = 0;

   /* Reset any prior pass.  If a download is currently in flight we
    * can't interrupt it (the task has no cancel hook), but our hook
    * slot in the upstream task module is single-shot — when the in-
    * flight download finishes it will reach for the slot we're about
    * to overwrite below in queue_next().  Belt and suspenders. */
   pastime_cores_clear_queue();

   if (!idents || count == 0)
   {
      g_state.state = PASTIME_CORES_DONE;
      return;
   }

   /* Filter to missing + deduped. */
   g_state.idents = (char**)calloc(count, sizeof(char*));
   if (!g_state.idents)
   {
      g_state.state = PASTIME_CORES_DONE;
      return;
   }
   for (i = 0; i < count; i++)
   {
      const char *ident = idents[i];
      size_t      j;
      bool        seen  = false;

      if (!ident || !*ident)
         continue;
      if (pastime_cores_is_installed(ident))
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
      g_state.state  = PASTIME_CORES_DONE;
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
   g_state.state         = PASTIME_CORES_AWAITING_LIST;
}

void pastime_cores_pump(void)
{
   core_updater_list_t *list;
   bool                 list_ready = false;

   if (g_state.state != PASTIME_CORES_AWAITING_LIST)
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
      pastime_cores_queue_next();
      return;
   }

   /* Empty / unfetched list.  Distinguishing "still in flight" from
    * "fetch failed" without another patch point isn't possible, so
    * fall back to a wall-clock timeout: dismiss the splash and let the
    * lazy-launch fallback handle individual missing cores later. */
   if (cpu_features_get_time_usec() - g_state.await_started
         > PASTIME_CORES_LIST_TIMEOUT_USEC)
   {
      RARCH_WARN("[Pastime] core updater list timed out; giving up\n");
      g_state.state = PASTIME_CORES_DONE;
   }
}

enum pastime_cores_state pastime_cores_get_state(void)
{
   return g_state.state;
}

const char *pastime_cores_get_progress(size_t *out_done, size_t *out_total)
{
   if (out_done)
      *out_done = g_state.cursor;
   if (out_total)
      *out_total = g_state.count;
   if (g_state.state != PASTIME_CORES_INSTALLING
         || g_state.cursor >= g_state.count)
      return NULL;
   return g_state.idents[g_state.cursor];
}

void pastime_cores_cancel(void)
{
   if (g_state.state == PASTIME_CORES_INACTIVE
         || g_state.state == PASTIME_CORES_DONE)
      return;
   if (g_state.state == PASTIME_CORES_AWAITING_LIST)
   {
      /* The list task may have already been freed by the task scheduler;
       * drop our pointer so a stale deref can't sneak in if the module is
       * re-entered before the next begin_boot_setup clears the queue. */
      g_state.list_task = NULL;
      g_state.state     = PASTIME_CORES_DONE;
      return;
   }
   /* INSTALLING: let the in-flight task finish; the callback will see
    * cancelled=true and short-circuit instead of queuing the next. */
   g_state.cancelled = true;
}
